// debug_bridge.cpp — desktop debug facade implementation (see debug_bridge.h). Dispatches on
// currentPlatform to reach each core's CPU registers / memory. Apple II (6502) is fully wired; the
// other platforms light up as their accessors are added (they currently report "(unsupported)").
#if defined(BOARD_DESKTOP)

#include "debug_bridge.h"
#include "../../emu.h"      // currentPlatform, PLATFORM_*, the 6502 globals (A/X/Y/PC/STP/SR, ram[]...),
                            // the soft-switch flags, read8(), and the Arduino ESP shim (ESP.restart())
#include <cstdlib>
#include <cstring>

void desktopUiSaveState();   // ui_imgui.cpp — flush window positions + settings before any process re-exec
static void rebootInto(int p);   // re-exec into platform p (defined below; used by dbgReset fallback)
// Per-platform in-process reset requests (serviced by each core's loop) — reboot the EMULATOR only.
namespace c64   { extern volatile bool c64ResetReq; }
namespace nes   { extern volatile bool nesResetReq; }
namespace atari { extern volatile bool atariResetReq; }

int dbgStepReq = 0;

// ---- heat map state ----
bool      g_dbgHeatOn = false;
uint32_t *g_dbgHeat[3] = {nullptr, nullptr, nullptr};

void dbgHeatEnable(bool on) {
  // Buffers are allocated once on first enable and kept for the session: disabling just clears the
  // flag (never frees), so the CPU thread can't deref a freed buffer mid-dbgBusTouch (no lock needed).
  if (on) {
    for (int k = 0; k < 3; k++)
      if (!g_dbgHeat[k]) g_dbgHeat[k] = (uint32_t *)calloc(0x10000, sizeof(uint32_t));
    g_dbgHeatOn = (g_dbgHeat[0] && g_dbgHeat[1] && g_dbgHeat[2]);
  } else {
    g_dbgHeatOn = false;
  }
}
bool dbgHeatEnabled() { return g_dbgHeatOn; }
void dbgHeatClear() {
  for (int k = 0; k < 3; k++) if (g_dbgHeat[k]) memset(g_dbgHeat[k], 0, 0x10000 * sizeof(uint32_t));
}
void dbgHeatDecay(float keep) {
  if (!g_dbgHeatOn) return;
  for (int k = 0; k < 3; k++)
    for (int i = 0; i < 0x10000; i++) g_dbgHeat[k][i] = (uint32_t)(g_dbgHeat[k][i] * keep);
}
const uint32_t *dbgHeatBuf(int kind) { return (kind >= 0 && kind < 3) ? g_dbgHeat[kind] : nullptr; }

// ---- disk read/write heat map ----
bool     g_dbgDiskHeatOn = false;
uint32_t g_dbgDiskHeat[DBG_DISK_TRACKS * DBG_DISK_BINS]  = {0};   // reads
uint32_t g_dbgDiskHeatW[DBG_DISK_TRACKS * DBG_DISK_BINS] = {0};   // writes
int      g_dbgDiskTrack = -1;
bool dbgDiskHeatSupported() { return currentPlatform == PLATFORM_APPLE2; }   // only the Disk II is hooked
void dbgDiskHeatEnable(bool on) { g_dbgDiskHeatOn = on; }
bool dbgDiskHeatEnabled() { return g_dbgDiskHeatOn; }
void dbgDiskHeatClear() {
  memset(g_dbgDiskHeat,  0, sizeof(g_dbgDiskHeat));
  memset(g_dbgDiskHeatW, 0, sizeof(g_dbgDiskHeatW));
  g_dbgDiskTrack = -1;
}
void dbgDiskHeatDecay(float keep) {
  for (int i = 0; i < DBG_DISK_TRACKS * DBG_DISK_BINS; i++) {
    g_dbgDiskHeat[i]  = (uint32_t)(g_dbgDiskHeat[i]  * keep);
    g_dbgDiskHeatW[i] = (uint32_t)(g_dbgDiskHeatW[i] * keep);
  }
}
// A track/sector floppy is mounted (-> circular disk view) vs an HD/block image (-> grid). The Disk II
// nibble hook only feeds floppy reads, so this is just !HdDisk on the Apple II.
bool dbgDiskIsFloppy() { return currentPlatform == PLATFORM_APPLE2 && !HdDisk; }

// ---- breakpoint / watchpoint / run-control state ----
bool    g_dbgBpAny = false;
bool    g_dbgBp[0x10000] = {false};
bool    g_dbgBreakArmed = true;
int     g_dbgRunToPC = -1;
int     g_dbgRunUntilSP = -1;
bool    g_dbgWatchAny = false;
uint8_t g_dbgWatch[0x10000] = {0};
int     g_dbgWatchHit = -1;

// ================================ execution control ===============================================
void dbgSetPaused(bool p) {
  paused = p;
  if (!p) { dbgStepReq = 0; g_dbgBreakArmed = false; }   // on resume, don't instantly re-break at the current PC
  else { g_dbgBreakArmed = true; g_dbgRunToPC = -1; g_dbgRunUntilSP = -1; }  // manual pause cancels pending runs
}
bool dbgIsPaused()        { return paused; }

bool dbgStepSupported() {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: case PLATFORM_C64:
    case PLATFORM_NES:    case PLATFORM_ATARI: return true;   // these cores honor dbgStepReq
    default: return false;
  }
}
void dbgStep() {
  if (!paused) paused = true;     // step implies paused
  if (dbgStepSupported()) dbgStepReq++;
}
// Apple II cold reboot WITHOUT re-exec: invalidate the Monitor power-up byte so the ROM reset handler
// does a full COLD start (scans slots + boots the disk), force the disk to re-open, then reset the
// 6502 to its reset vector. The desktop app / ImGui windows stay exactly where they are.
static void a2ColdReboot() {
  bool wasPaused = paused;
  paused = true;
  ram[0x3F4] = 0x00;            // power-up byte mismatch -> Monitor cold-starts (re-boots a slot)
  diskChanged = true;          // next $C0EC read re-opens the disk image (re-seek from track 0)
  PC = read16(0xFFFC); STP = 0xFD; SR = 0x04;   // 6502 RESET
  paused = wasPaused;
}

// "Reboot" reboots the EMULATED MACHINE ONLY — it must NOT restart the desktop application (that would
// move the windows). In-process where the core supports it; the rest re-exec (state saved first).
void dbgReset() {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: a2ColdReboot();             break;
    case PLATFORM_C64:    c64::c64ResetReq   = true;  break;
    case PLATFORM_NES:    nes::nesResetReq   = true;  break;
    case PLATFORM_ATARI:  atari::atariResetReq = true; break;
    default:              rebootInto(currentPlatform); break;   // MSX/SMS/PCXT/tiny386/IIgs: re-exec
  }
}

// --- machine model + adjustable clock (Apple II) ---
bool dbgAppleModelSupported() { return currentPlatform == PLATFORM_APPLE2; }
bool dbgGetAppleIIe() { return AppleIIe; }
void dbgSetAppleIIe(bool iie) {
  if (currentPlatform != PLATFORM_APPLE2 || iie == (bool)AppleIIe) return;
  AppleIIe = iie;
  activeFlags = AppleIIe ? flagsIIe : flagsIIplus;   // 6502 flag behavior differs (IIe vs II+)
  saveConfig();
  a2ColdReboot();                                    // re-boot so it comes up as the selected model
}
bool  dbgClockSupported()  { return currentPlatform == PLATFORM_APPLE2; }
bool  dbgGetThrottle()     { return !Fast1MhzSpeed; }              // throttled = the paced 1 MHz path
void  dbgSetThrottle(bool on) { Fast1MhzSpeed = !on; }
float dbgGetClockMhz()     { return appleClockMhz; }
void  dbgSetClockMhz(float mhz) { if (mhz < 0.05f) mhz = 0.05f; if (mhz > 16.0f) mhz = 16.0f; appleClockMhz = mhz; }
float dbgClockDefaultMhz() { return 1.0f; }                        // stock Apple II 6502 clock
float dbgGetMeasuredMhz()  { return appleMeasuredMhz; }

// Higher-level run control — only Apple II's cpuLoop carries the run-target checks so far.
bool dbgRunControlSupported() { return currentPlatform == PLATFORM_APPLE2; }
void dbgStepOver() {
  if (!dbgRunControlSupported()) { dbgStep(); return; }
  if (dbgPeek(PC) == 0x20) {                  // JSR abs -> run the subroutine, break at the return addr
    g_dbgRunToPC = (PC + 3) & 0xFFFF;
    dbgSetPaused(false);
  } else {
    dbgStep();                                // anything else -> ordinary step-into
  }
}
void dbgStepOut() {
  if (!dbgRunControlSupported()) return;
  g_dbgRunUntilSP = STP;                       // pause once SP rises above the current frame (RTS popped)
  dbgSetPaused(false);
}
void dbgRunTo(uint32_t addr) {
  if (!dbgRunControlSupported()) return;
  g_dbgRunToPC = (int)(addr & 0xFFFF);
  dbgSetPaused(false);
}
bool dbgSoftResetSupported() { return currentPlatform == PLATFORM_APPLE2; }
void dbgSoftReset() {
  if (!dbgSoftResetSupported()) return;
  g_dbgRunToPC = -1; g_dbgRunUntilSP = -1;
  PC = read16(0xFFFC); STP = 0xFD; SR |= 0x04;  // 6502 RESET: load the reset vector, init SP, set I
}

// ================================ platform control ================================================
// EMU_PLATFORM string the eprom.cpp parser accepts (so a re-exec boots this platform + skips splash).
static const char *platEnvName(int p) {
  switch (p) {
    case PLATFORM_APPLE2: return "apple2"; case PLATFORM_C64:  return "c64";
    case PLATFORM_NES:    return "nes";    case PLATFORM_ATARI: return "atari";
    case PLATFORM_IIGS:   return "iigs";   case PLATFORM_MSX:   return "msx";
    case PLATFORM_SMS:    return "sms";    case PLATFORM_PCXT:  return "pcxt";
    case PLATFORM_TINY386: return "tiny386"; default: return "apple2";
  }
}
static void rebootInto(int p) {
  desktopUiSaveState();    // CRITICAL: persist window positions + settings BEFORE the process re-execs
  static char buf[40];
  snprintf(buf, sizeof(buf), "EMU_PLATFORM=%s", platEnvName(p));
  putenv(buf);             // re-exec (below) inherits the env -> boots p, skips the splash
  ESP.restart();
}

int dbgPlatform()      { return currentPlatform; }
int dbgPlatformCount() { return PLATFORM_TINY386 + 1; }
const char *dbgPlatformName(int p) {
  switch (p) {
    case PLATFORM_APPLE2: return "Apple II"; case PLATFORM_C64:  return "Commodore 64";
    case PLATFORM_NES:    return "NES";      case PLATFORM_ATARI: return "Atari 2600";
    case PLATFORM_IIGS:   return "Apple IIGS"; case PLATFORM_MSX: return "MSX";
    case PLATFORM_SMS:    return "Master System"; case PLATFORM_PCXT: return "PC-XT (8086)";
    case PLATFORM_TINY386: return "PC (i386)"; default: return "?";
  }
}
void dbgSwitchPlatform(int p) {
  if (p < 0 || p > PLATFORM_TINY386 || p == currentPlatform) return;
  currentPlatform = p;
  saveConfig();            // persist so the re-exec'd setup() inits the chosen core
  rebootInto(p);
}

// Mount/load an SD file for the current platform. Most cores hot-load (no reboot); Apple II/IIGS
// re-exec so their boot path mounts the image (their disk path can't safely hot-swap mid-run).
bool dbgLoadFile(const char *path) {
  bool ok = false;
  switch (currentPlatform) {
    case PLATFORM_C64:     ok = c64LoadSelected(path); break;
    case PLATFORM_NES:     ok = nesLoadSelected(path); break;
    case PLATFORM_ATARI:   ok = atariLoadSelected(path); break;
    case PLATFORM_MSX:     ok = msxLoadSelected(path); break;
    case PLATFORM_SMS:     ok = smsLoadSelected(path); break;
    case PLATFORM_PCXT:    ok = pcxtMountA(path); break;
    case PLATFORM_TINY386: ok = tiny386MountC(path); break;
    case PLATFORM_IIGS:    iigsLoadDisk(path); return true;   // reboots internally (persists first)
    case PLATFORM_APPLE2:
    default:
      apple2InsertDisk(path);   // hot-swap, NO reboot — the running software reads it on next access
      ok = true; break;
  }
  if (ok) saveConfig();   // persist the just-loaded image so it auto-mounts next boot
  return ok;
}
const char *dbgFileExts() {
  switch (currentPlatform) {
    case PLATFORM_APPLE2:  return "dsk nib do po woz 2mg hdv img";
    case PLATFORM_C64:     return "d64 prg t64 crt";
    case PLATFORM_NES:     return "nes";
    case PLATFORM_ATARI:   return "a26 bin";
    case PLATFORM_IIGS:    return "dsk po 2mg hdv";
    case PLATFORM_MSX:     return "rom dsk mx1 mx2";
    case PLATFORM_SMS:     return "sms bin";
    case PLATFORM_PCXT:    return "img dsk ima";
    case PLATFORM_TINY386: return "img vhd";
    default: return "";
  }
}

// ================================ Apple II (6502) =================================================
// Side-effect-free read: mirrors read8()'s RAM/ROM decode but NEVER touches the $C000-$C0FF
// soft-switches or the $C100-$CFFF slot-ROM bank flags (which a memory viewer must not perturb).
static uint8_t a2peek(uint16_t a) {
  if (a < 0x0200) return AltZPOn_Off ? auxzp[a] : zp[a];
  if (a < 0xC000) {
    if (!Store80On_Off) return RAMReadOn_Off ? auxram[a] : ram[a];
    if (a >= 0x0400 && a < 0x0800) return (!Page1_Page2) ? auxram[a] : ram[a];     // text page
    if (a >= 0x2000 && a < 0x4000) {                                               // graphics page
      if (LoRes_HiRes)            return RAMReadOn_Off ? auxram[a] : ram[a];
      return (!Page1_Page2) ? auxram[a] : ram[a];
    }
    return RAMReadOn_Off ? auxram[a] : ram[a];
  }
  if (a < 0xD000) return 0;            // $C000-$CFFF: I/O + slot ROM — unsafe to read, shown as 0
  return read8(a);                     // $D000-$FFFF: language-card / ROM — side-effect-free
}
static void a2poke(uint16_t a, uint8_t v) {
  if (a < 0x0200) { (AltZPOn_Off ? auxzp : zp)[a] = v; return; }
  if (a < 0xC000) {
    if (!Store80On_Off) { (RAMReadOn_Off ? auxram : ram)[a] = v; return; }
    if (a >= 0x0400 && a < 0x0800) { ((!Page1_Page2) ? auxram : ram)[a] = v; return; }
    if (a >= 0x2000 && a < 0x4000) {
      if (LoRes_HiRes) { (RAMReadOn_Off ? auxram : ram)[a] = v; return; }
      ((!Page1_Page2) ? auxram : ram)[a] = v; return;
    }
    (RAMReadOn_Off ? auxram : ram)[a] = v; return;
  }
  // $C000+ (I/O / ROM): ignore pokes
}

// ================================ CPU identity / registers ========================================
const char *dbgCpuName() {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: return "MOS 6502";
    case PLATFORM_C64:    return "MOS 6510";
    case PLATFORM_NES:    return "Ricoh 2A03";
    case PLATFORM_ATARI:  return "MOS 6507";
    case PLATFORM_IIGS:   return "WDC 65C816";
    case PLATFORM_MSX:    return "Zilog Z80";
    case PLATFORM_SMS:    return "Zilog Z80";
    case PLATFORM_PCXT:   return "Intel 8086";
    case PLATFORM_TINY386: return "Intel i386";
    default: return "(unsupported)";
  }
}

int dbgGetRegs(DbgReg *o, int max) {
  int n = 0;
  auto add = [&](const char *nm, uint32_t v, uint8_t bits) { if (n < max) { o[n++] = {nm, v, bits}; } };
  switch (currentPlatform) {
    case PLATFORM_APPLE2:
      add("PC", PC, 16); add("A", A, 8); add("X", X, 8); add("Y", Y, 8);
      add("SP", 0x0100 | STP, 16); add("P", SR, 8);
      break;
    default: break;   // other platforms: registers not yet wired
  }
  return n;
}

uint32_t dbgGetPC() {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: return PC;
    default: return 0;
  }
}

bool dbgGetFlags(const char *const **labels, uint32_t *value, int *count) {
  static const char *const p6502[8] = {"N","V","-","B","D","I","Z","C"};   // MSB..LSB
  switch (currentPlatform) {
    case PLATFORM_APPLE2:
      if (labels) *labels = p6502;
      if (value)  *value  = SR;
      if (count)  *count  = 8;
      return true;
    default: return false;
  }
}

// ================================ memory ==========================================================
bool dbgMemReadable() {
  return currentPlatform == PLATFORM_APPLE2;   // extended per-platform as cores are wired
}
uint32_t dbgMemSize() {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: return 0x10000;   // 64K CPU address space
    default: return 0;
  }
}
uint8_t dbgPeek(uint32_t a) {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: return a2peek((uint16_t)a);
    default: return 0;
  }
}
bool dbgPokeSupported() { return currentPlatform == PLATFORM_APPLE2; }
void dbgPoke(uint32_t a, uint8_t v) {
  switch (currentPlatform) {
    case PLATFORM_APPLE2: a2poke((uint16_t)a, v); break;
    default: break;
  }
}

// ================================ breakpoints =====================================================
// Only Apple II's cpuLoop carries the dbgBpShouldBreak() hook so far; extending to C64/NES/Atari is
// the same one-line add in their cpuLoop pause spins.
bool dbgBpSupported() { return currentPlatform == PLATFORM_APPLE2; }
bool dbgBpAt(uint32_t a) { return g_dbgBp[a & 0xFFFF]; }
void dbgBpToggle(uint32_t a) {
  a &= 0xFFFF;
  g_dbgBp[a] = !g_dbgBp[a];
  if (g_dbgBp[a]) g_dbgBpAny = true;
  else { g_dbgBpAny = false; for (int i = 0; i < 0x10000; i++) if (g_dbgBp[i]) { g_dbgBpAny = true; break; } }
}
void dbgBpClearAll() { for (int i = 0; i < 0x10000; i++) g_dbgBp[i] = false; g_dbgBpAny = false; }
int dbgBpList(uint16_t *out, int max) {
  int n = 0;
  for (int i = 0; i < 0x10000 && n < max; i++) if (g_dbgBp[i]) out[n++] = (uint16_t)i;
  return n;
}

// ================================ watchpoints =====================================================
bool dbgWatchSupported() { return currentPlatform == PLATFORM_APPLE2; }   // needs the bus hooks (apple2)
uint8_t dbgWatchAt(uint32_t a) { return g_dbgWatch[a & 0xFFFF]; }
void dbgWatchToggle(uint32_t a, uint8_t kindMask) {
  a &= 0xFFFF;
  g_dbgWatch[a] ^= kindMask;
  g_dbgWatchAny = false;
  for (int i = 0; i < 0x10000; i++) if (g_dbgWatch[i]) { g_dbgWatchAny = true; break; }
}
void dbgWatchClearAll() { for (int i = 0; i < 0x10000; i++) g_dbgWatch[i] = 0; g_dbgWatchAny = false; g_dbgWatchHit = -1; }

// ================================ I/O / soft-switch state =========================================
// Apple II soft switches. Polarity = the flags' "true = first token" convention (verified against
// memory.cpp read8 + video.cpp render conditions). `active` highlights the non-default state.
int dbgGetIoState(DbgFlag *o, int max) {
  if (currentPlatform != PLATFORM_APPLE2) return 0;
  int n = 0;
  auto add = [&](const char *lbl, const char *val, bool act) { if (n < max) o[n++] = {lbl, val, act}; };
  add("Model",     AppleIIe ? "IIe" : "II+", false);
  add("Mode",      Graphics_Text ? "GRAPHICS" : "TEXT", Graphics_Text);
  add("Display",   DisplayFull_Split ? "FULL" : "MIXED", !DisplayFull_Split);
  add("Page",      Page1_Page2 ? "1" : "2", !Page1_Page2);
  add("Res",       LoRes_HiRes ? "LORES" : "HIRES", false);
  add("DHGR",      DHiResOn_Off ? "ON" : "off", DHiResOn_Off);
  add("Columns",   Cols40_80 ? "40" : "80", !Cols40_80);
  add("80STORE",   Store80On_Off ? "ON" : "off", Store80On_Off);
  add("RAMRD",     RAMReadOn_Off ? "aux" : "main", RAMReadOn_Off);
  add("RAMWRT",    RAMWriteOn_Off ? "aux" : "main", RAMWriteOn_Off);
  add("ALTZP",     AltZPOn_Off ? "aux" : "main", AltZPOn_Off);
  add("INTCXROM",  IntCXRomOn_Off ? "internal" : "slot", IntCXRomOn_Off);
  add("SLOTC3ROM", SlotC3RomOn_Off ? "slot" : "internal", SlotC3RomOn_Off);
  // language card ($D000-$FFFF bank); IIe and II+ keep separate flags
  bool lcRead  = AppleIIe ? IIEMemoryBankReadRAM_ROM      : MemoryBankReadRAM_ROM;
  bool lcWrite = AppleIIe ? IIEMemoryBankWriteRAM_NoWrite : MemoryBankWriteRAM_NoWrite;
  bool lcBank1 = AppleIIe ? IIEMemoryBankBankSelect1_2    : MemoryBankBankSelect1_2;
  add("LC read",   lcRead ? "RAM" : "ROM", lcRead);
  add("LC write",  lcWrite ? "RAM" : "off", lcWrite);
  add("LC bank",   lcBank1 ? "1" : "2", false);
  return n;
}

// ================================ 6502 disassembler ===============================================
namespace {
enum AM { IMP, ACC, IMM, ZP, ZPX, ZPY, IZX, IZY, ABS, ABX, ABY, IND, REL };
struct Op { const char *m; uint8_t am; };
const Op OPS[256] = {
  /*00*/ {"BRK",IMP},{"ORA",IZX},{"???",IMP},{"???",IMP},{"???",IMP},{"ORA",ZP},{"ASL",ZP},{"???",IMP},{"PHP",IMP},{"ORA",IMM},{"ASL",ACC},{"???",IMP},{"???",IMP},{"ORA",ABS},{"ASL",ABS},{"???",IMP},
  /*10*/ {"BPL",REL},{"ORA",IZY},{"???",IMP},{"???",IMP},{"???",IMP},{"ORA",ZPX},{"ASL",ZPX},{"???",IMP},{"CLC",IMP},{"ORA",ABY},{"???",IMP},{"???",IMP},{"???",IMP},{"ORA",ABX},{"ASL",ABX},{"???",IMP},
  /*20*/ {"JSR",ABS},{"AND",IZX},{"???",IMP},{"???",IMP},{"BIT",ZP},{"AND",ZP},{"ROL",ZP},{"???",IMP},{"PLP",IMP},{"AND",IMM},{"ROL",ACC},{"???",IMP},{"BIT",ABS},{"AND",ABS},{"ROL",ABS},{"???",IMP},
  /*30*/ {"BMI",REL},{"AND",IZY},{"???",IMP},{"???",IMP},{"???",IMP},{"AND",ZPX},{"ROL",ZPX},{"???",IMP},{"SEC",IMP},{"AND",ABY},{"???",IMP},{"???",IMP},{"???",IMP},{"AND",ABX},{"ROL",ABX},{"???",IMP},
  /*40*/ {"RTI",IMP},{"EOR",IZX},{"???",IMP},{"???",IMP},{"???",IMP},{"EOR",ZP},{"LSR",ZP},{"???",IMP},{"PHA",IMP},{"EOR",IMM},{"LSR",ACC},{"???",IMP},{"JMP",ABS},{"EOR",ABS},{"LSR",ABS},{"???",IMP},
  /*50*/ {"BVC",REL},{"EOR",IZY},{"???",IMP},{"???",IMP},{"???",IMP},{"EOR",ZPX},{"LSR",ZPX},{"???",IMP},{"CLI",IMP},{"EOR",ABY},{"???",IMP},{"???",IMP},{"???",IMP},{"EOR",ABX},{"LSR",ABX},{"???",IMP},
  /*60*/ {"RTS",IMP},{"ADC",IZX},{"???",IMP},{"???",IMP},{"???",IMP},{"ADC",ZP},{"ROR",ZP},{"???",IMP},{"PLA",IMP},{"ADC",IMM},{"ROR",ACC},{"???",IMP},{"JMP",IND},{"ADC",ABS},{"ROR",ABS},{"???",IMP},
  /*70*/ {"BVS",REL},{"ADC",IZY},{"???",IMP},{"???",IMP},{"???",IMP},{"ADC",ZPX},{"ROR",ZPX},{"???",IMP},{"SEI",IMP},{"ADC",ABY},{"???",IMP},{"???",IMP},{"???",IMP},{"ADC",ABX},{"ROR",ABX},{"???",IMP},
  /*80*/ {"???",IMP},{"STA",IZX},{"???",IMP},{"???",IMP},{"STY",ZP},{"STA",ZP},{"STX",ZP},{"???",IMP},{"DEY",IMP},{"???",IMP},{"TXA",IMP},{"???",IMP},{"STY",ABS},{"STA",ABS},{"STX",ABS},{"???",IMP},
  /*90*/ {"BCC",REL},{"STA",IZY},{"???",IMP},{"???",IMP},{"STY",ZPX},{"STA",ZPX},{"STX",ZPY},{"???",IMP},{"TYA",IMP},{"STA",ABY},{"TXS",IMP},{"???",IMP},{"???",IMP},{"STA",ABX},{"???",IMP},{"???",IMP},
  /*A0*/ {"LDY",IMM},{"LDA",IZX},{"LDX",IMM},{"???",IMP},{"LDY",ZP},{"LDA",ZP},{"LDX",ZP},{"???",IMP},{"TAY",IMP},{"LDA",IMM},{"TAX",IMP},{"???",IMP},{"LDY",ABS},{"LDA",ABS},{"LDX",ABS},{"???",IMP},
  /*B0*/ {"BCS",REL},{"LDA",IZY},{"???",IMP},{"???",IMP},{"LDY",ZPX},{"LDA",ZPX},{"LDX",ZPY},{"???",IMP},{"CLV",IMP},{"LDA",ABY},{"TSX",IMP},{"???",IMP},{"LDY",ABX},{"LDA",ABX},{"LDX",ABY},{"???",IMP},
  /*C0*/ {"CPY",IMM},{"CMP",IZX},{"???",IMP},{"???",IMP},{"CPY",ZP},{"CMP",ZP},{"DEC",ZP},{"???",IMP},{"INY",IMP},{"CMP",IMM},{"DEX",IMP},{"???",IMP},{"CPY",ABS},{"CMP",ABS},{"DEC",ABS},{"???",IMP},
  /*D0*/ {"BNE",REL},{"CMP",IZY},{"???",IMP},{"???",IMP},{"???",IMP},{"CMP",ZPX},{"DEC",ZPX},{"???",IMP},{"CLD",IMP},{"CMP",ABY},{"???",IMP},{"???",IMP},{"???",IMP},{"CMP",ABX},{"DEC",ABX},{"???",IMP},
  /*E0*/ {"CPX",IMM},{"SBC",IZX},{"???",IMP},{"???",IMP},{"CPX",ZP},{"SBC",ZP},{"INC",ZP},{"???",IMP},{"INX",IMP},{"SBC",IMM},{"NOP",IMP},{"???",IMP},{"CPX",ABS},{"SBC",ABS},{"INC",ABS},{"???",IMP},
  /*F0*/ {"BEQ",REL},{"SBC",IZY},{"???",IMP},{"???",IMP},{"???",IMP},{"SBC",ZPX},{"INC",ZPX},{"???",IMP},{"SED",IMP},{"SBC",ABY},{"???",IMP},{"???",IMP},{"???",IMP},{"SBC",ABX},{"INC",ABX},{"???",IMP},
};
} // namespace

static int disasm6502(uint16_t pc, char *out, int n) {
  uint8_t op = dbgPeek(pc);
  const Op &o = OPS[op];
  uint8_t b1 = dbgPeek((pc + 1) & 0xFFFF), b2 = dbgPeek((pc + 2) & 0xFFFF);
  uint16_t w = (uint16_t)(b1 | (b2 << 8));
  switch (o.am) {
    case IMP: snprintf(out, n, "%s",          o.m);     return 1;
    case ACC: snprintf(out, n, "%s A",        o.m);     return 1;
    case IMM: snprintf(out, n, "%s #$%02X",   o.m, b1); return 2;
    case ZP:  snprintf(out, n, "%s $%02X",    o.m, b1); return 2;
    case ZPX: snprintf(out, n, "%s $%02X,X",  o.m, b1); return 2;
    case ZPY: snprintf(out, n, "%s $%02X,Y",  o.m, b1); return 2;
    case IZX: snprintf(out, n, "%s ($%02X,X)",o.m, b1); return 2;
    case IZY: snprintf(out, n, "%s ($%02X),Y",o.m, b1); return 2;
    case ABS: snprintf(out, n, "%s $%04X",    o.m, w);  return 3;
    case ABX: snprintf(out, n, "%s $%04X,X",  o.m, w);  return 3;
    case ABY: snprintf(out, n, "%s $%04X,Y",  o.m, w);  return 3;
    case IND: snprintf(out, n, "%s ($%04X)",  o.m, w);  return 3;
    case REL: { uint16_t t = (uint16_t)(pc + 2 + (int8_t)b1); snprintf(out, n, "%s $%04X", o.m, t); return 2; }
  }
  return 1;
}

bool dbgDisasmSupported() {
  if (!dbgMemReadable()) return false;        // needs a working peek (lights up with each platform)
  switch (currentPlatform) {
    case PLATFORM_APPLE2: case PLATFORM_C64: case PLATFORM_NES: case PLATFORM_ATARI: return true;
    default: return false;
  }
}
int dbgDisasm(uint32_t addr, char *out, int outsz) {
  if (dbgDisasmSupported()) return disasm6502((uint16_t)addr, out, outsz);
  if (out && outsz) out[0] = 0;
  return 1;
}

#endif // BOARD_DESKTOP
