// pcxt.cpp - device-side glue for the PC-XT (Intel 8086) platform: memory
// allocation, the core-1 run loop, the core-0 CGA render push, USB-keyboard ->
// XT scancode injection, and the settings (disk browser / mount) hooks. This is
// the only PC-XT file that pulls in emu.h (Arduino/board); the machine core in
// src/pcxt/fabgl/ stays host-portable. Mirrors sms.cpp.
//
// Boot path: the BIOS ROM (embedded in src/pcxt/fabgl/biosrom.h, installed by
// BIOS::init) runs from reset; POST text appears in the CGA buffer (rendered
// here) before any disk is touched.

#include "../../emu.h"
#include "pcxt.h"
#include "fabgl/machine.h"
#include <dirent.h>
#include <string>

// 1 MB main RAM (PSRAM) + 64 KB CGA/video window (internal: read every frame).
static uint8_t* pcRam   = nullptr;
static uint8_t* pcVRam  = nullptr;
static volatile bool pcResetReq = false;
static bool pcInitDone = false;

static uint8_t* pcAllocFast(size_t n) {                 // internal SRAM first, PSRAM fallback
  uint8_t* p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) p = (uint8_t*)ps_malloc(n);
  return p;
}
static bool pcEndsCI(const std::string& s, const char* suf) {
  size_t n = strlen(suf); if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++)
    if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)suf[i])) return false;
  return true;
}
static bool pcIsDiskImage(const std::string& s) {
  return pcEndsCI(s, ".img") || pcEndsCI(s, ".ima") || pcEndsCI(s, ".dsk") ||
         pcEndsCI(s, ".vhd") || pcEndsCI(s, ".hdd");
}

// A real PC disk image has the 0x55AA boot signature at offset 510 (FAT boot sector / MBR). The
// user's IIgs/Apple .img/.dsk images lack it, so this keeps them OUT of the PC browser -> they can't
// be mounted by accident and corrupted by write-back. busTake() guards the HSPI (touch shares it).
static bool pcLooksLikePcDisk(const char* path) {
  bool ok = false;
  busTake();
  File f = FSTYPE.open(path, FILE_READ);
  if (f) {
    if (f.size() >= 512 && f.seek(510)) {
      uint8_t s[2] = {0, 0};
      if (f.read(s, 2) == 2) ok = (s[0] == 0x55 && s[1] == 0xAA);
    }
    f.close();
  }
  busGive();
  return ok;
}

// Scan SD root for PC disk images into pcFiles (extension match + 0x55AA boot signature).
#define PCXT_MAX_FILES 200
void loadPcxtFilesSync() {
  pcFiles.clear();
  pcFiles.reserve(PCXT_MAX_FILES);
  DIR* dp = opendir(SD_VFS_ROOT);
  if (dp) {
    struct dirent* de; int scanned = 0;
    while ((de = readdir(dp)) != nullptr) {
      if (de->d_type == DT_DIR) continue;
      std::string nm = de->d_name;
      if (pcIsDiskImage(nm)) {
        std::string full = std::string("/") + nm;
        if (pcLooksLikePcDisk(full.c_str())) pcFiles.push_back(full);
      }
      if ((++scanned & 0x3f) == 0) ::uiDirScanProgress((int)pcFiles.size());
      if ((int)pcFiles.size() >= PCXT_MAX_FILES) break;
    }
    closedir(dp);
  }
  sprintf(buf, "PCXT: %d PC disk image(s) on SD root", (int)pcFiles.size());
  printLog(buf);
}

// ---- SD/File disk backend ----
// The stdio FILE* path (fopen/fseek/ftell over the ESP32 SD VFS) reported size 0 and
// unreliable seeks, so disk images go through the Arduino SD File API (card-relative
// paths like "/dos.img"), which has reliable size()/seek()/read()/write(). Called from
// the BIOS INT 13h handler, already wrapped in busTake/busGive by the machine.
static File pcDiskPool[4];
static bool pcRwSafe = false;                  // set by the setup probe: is "r+" non-truncating here?

static void* pcDiskOpen(const char* path, uint64_t* sizeOut) {
  int slot = -1;
  for (int i = 0; i < 4; i++) if (!pcDiskPool[i]) { slot = i; break; }
  if (slot < 0) return nullptr;
  File f = pcRwSafe ? FSTYPE.open(path, "r+")   // read+write (no truncate) when probe says it's safe
                    : FSTYPE.open(path, FILE_READ);
  if (!f) f = FSTYPE.open(path, FILE_READ);     // fall back to read-only if r+ is refused
  if (!f) { sprintf(buf, "pcDiskOpen: open FAILED %s", path); printLog(buf); return nullptr; }
  pcDiskPool[slot] = f;
  if (sizeOut) *sizeOut = (uint64_t)pcDiskPool[slot].size();
  return &pcDiskPool[slot];
}

static int pcDiskIo(void* ctx, uint64_t pos, uint8_t* bufp, uint32_t count, bool write) {
  if (!ctx) return 0;
  File* f = (File*)ctx;
  if (!f->seek((uint32_t)pos)) return 0;
  if (write) { int n = (int)f->write(bufp, count); f->flush(); return n; }  // flush so writes persist
  return (int)f->read(bufp, count);
}

static void pcDiskClose(void* ctx) {
  if (ctx) ((File*)ctx)->close();
}

// Mount `path` into a specific drive slot (0 = A: floppy, 2 = C: hard disk), WITHOUT rebooting.
// Updates that slot's saved name and the boot order (floppy first if present, else hard disk).
// A: media changes are seen live by DOS; a newly-added C: is recognised only after a reboot.
static bool pcMountInto(const char* path, int slot) {
  if (!pcInitDone) return false;
  std::string p = path ? path : "";
  if (!pcIsDiskImage(p)) { printLog("PCXT: unsupported file (.img/.ima/.dsk/.vhd)"); return false; }
  if (!pcLooksLikePcDisk(path)) { sprintf(buf, "PCXT: %s is not a PC disk (no 55AA) - refused", path); printLog(buf); return false; }
  g_pcxtMachine.setDriveImage(slot, path);
  if (slot == 0) selectedPcFileName = path; else selectedPcHdFileName = path;
  g_pcxtMachine.setBootDrive(g_pcxtMachine.disk(0) ? 0 : 2);   // floppy first, else boot the hard disk
  sprintf(buf, "PCXT: mounted %s into %s", path, slot == 0 ? "A:" : "C:");
  printLog(buf);
  return true;
}

// ============================ platform entry points =============================================
void pcxtSetup() {
  printLog("PCXT Setup... (Intel 8086 + CGA, BIOS = 8086tiny-plus)");

  menuScreen = (unsigned char*)malloc(0x546);
  menuColor  = (unsigned char*)malloc(0x546);

  pcRam  = (uint8_t*)ps_malloc(PCXT_RAM_SIZE);            // 1 MB main RAM -> PSRAM
  pcVRam = pcAllocFast(PCXT_VIDEOMEM_SIZE);               // 64 KB video RAM -> internal preferred
  if (!pcRam || !pcVRam) {
    sprintf(buf, "PCXT: ALLOC FAIL ram=%p vram=%p (need 1MB PSRAM + 64KB)", pcRam, pcVRam);
    printLog(buf);
    return;
  }

  g_pcxtMachine.setMemoryBuffers(pcRam, pcVRam);
  g_pcxtMachine.setBootDrive(0);                          // floppy A: by default
  g_pcxtMachine.init();                                   // installs BIOS ROM, wires chipset
  g_pcxtMachine.setDiskBackend(pcDiskOpen, pcDiskIo, pcDiskClose);   // SD/File disk backend
  // disk INT 13h runs on core 1; serialize SD/HSPI against core-0 touch/render
  g_pcxtMachine.setDiskLock([]{ busTake(); }, []{ busGive(); });
  // PC-speaker: mirror PIT ch2 freq + port-0x61 gate into globals the audio ISR reads (speaker.cpp)
  g_pcxtMachine.setSpeakerCallback([](int freq, bool on){ g_pcSpkFreq = freq; g_pcSpkOn = on; });
  pcInitDone = true;

  // Probe whether the SD library's "r+" preserves the file size (does NOT truncate). Only then enable
  // write-back to disk images (so DOS can save/install); otherwise keep them read-only to protect data.
  // Uses a throwaway temp file, so no risk to user images.
  {
    const char* tp = "/pcxt_wt.tmp";
    File w = FSTYPE.open(tp, FILE_WRITE);
    if (w) {
      uint8_t z[8] = {1,2,3,4,5,6,7,8};
      w.write(z, 8); w.close();
      File rw = FSTYPE.open(tp, "r+");
      pcRwSafe = (rw && rw.size() == 8);
      if (rw) rw.close();
      FSTYPE.remove(tp);
    }
    printLog(pcRwSafe ? "PCXT: disk write-back ENABLED (r+ is non-truncating)"
                      : "PCXT: disk READ-ONLY (r+ truncates on this SD)");
  }

  if (selectedPcFileName.length() > 1 && selectedPcFileName != "/")
    pcMountInto(selectedPcFileName.c_str(), 0);     // A: floppy
  if (selectedPcHdFileName.length() > 1 && selectedPcHdFileName != "/")
    pcMountInto(selectedPcHdFileName.c_str(), 2);   // C: hard disk
  g_pcxtMachine.setBootDrive(g_pcxtMachine.disk(0) ? 0 : 2);

  sprintf(buf, "PCXT ready. internal free=%u, spiram free=%u",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  printLog(buf);
}

// The 8086 runs continuously; the PIT (real-time via FRC1Timer) drives IRQ0 at
// 18.2 Hz independent of emulation speed, so there is no fixed frame to pace. We
// just run the CPU in chunks and yield so core 0 can render and the watchdog is
// fed. A one-time boot benchmark reports the effective 8086 speed.
void pcxtLoop() {
  if (!pcInitDone) { for (;;) vTaskDelay(pdMS_TO_TICKS(200)); }

  // uncapped benchmark (~0.5 s real time)
  {
    uint32_t t0 = millis();
    uint64_t iters = 0;
    while ((uint32_t)(millis() - t0) < 500) { g_pcxtMachine.run(20000); iters += 20000; }
    uint32_t dt = millis() - t0;
    // rough: assume ~4 cycles/instruction average for an 8086 mix
    double instrPerSec = dt > 0 ? (double)iters / ((double)dt / 1000.0) : 0.0;
    pcMeasuredMhz = (float)(instrPerSec * 4.0 / 1e6);     // ~equiv MHz (real XT 8088 ~4.77)
    sprintf(buf, "PCXT: ~%.2f equiv-MHz (%.0f kIPS); real XT = 4.77", pcMeasuredMhz, instrPerSec / 1000.0);
    printLog(buf);
  }

  for (;;) {
    if (OptionsWindow) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    if (pcResetReq)    { pcResetReq = false; g_pcxtMachine.trigReset(); }
    g_pcxtMachine.run(40000);
    vTaskDelay(1);          // feed WDT, let core 0 render
  }
}

// ---- CGA text render (M1: 80x25 / 40x25 text only; graphics modes come in M3) ----
// Reads the CGA text buffer at videoMemory()+0x8000 (char at even byte, attribute
// odd) and blits with the built-in 6x8 font, like iigsRenderText. 80 cols * 6px =
// 480px = full native panel width.
static const uint16_t kCgaRgb565[16] = {
  0x0000, 0x0015, 0x0540, 0x0555, 0xA800, 0xA815, 0xAAA0, 0xAD55,
  0x52AA, 0x52BF, 0x57EA, 0x57FF, 0xFAAA, 0xFABF, 0xFFEA, 0xFFFF
};

static uint16_t* pcGfxScratch = nullptr;   // 320x8 RGB565 band for graphics modes

// ---- CGA graphics render (M3): 320x200x4 and 640x200x2 ----
// CGA graphics memory is interleaved: even scanlines at 0xB8000, odd at 0xBA000 (+0x2000), each
// bank 100 lines of 80 bytes. We decode straight to RGB565 8-line bands and push them centered
// in the 320-logical video rect (fill-screen scales to the panel), like the C64 path.
static void pcxtRenderGraphics(fabgl::GraphicsAdapter::Emulation emu) {
  if (!pcGfxScratch) pcGfxScratch = (uint16_t*)malloc(320 * 8 * sizeof(uint16_t));
  if (!pcGfxScratch) return;
  fabgl::GraphicsAdapter* ga = g_pcxtMachine.graphicsAdapter();
  const uint8_t* vm = g_pcxtMachine.videoMemory() + 0x8000;
  bool mode640 = (emu == fabgl::GraphicsAdapter::Emulation::PC_Graphics_640x200_2Colors);

  uint16_t pal[4];
  if (!mode640) {
    static const uint8_t PC[4][3] = {{2,4,6},{10,12,14},{3,5,7},{11,13,15}};  // CGA 4-colour palettes
    int pi = ga->graphPalette() & 3;
    pal[0] = kCgaRgb565[ga->graphBackgroundIndex() & 0x0F];
    pal[1] = kCgaRgb565[PC[pi][0]];
    pal[2] = kCgaRgb565[PC[pi][1]];
    pal[3] = kCgaRgb565[PC[pi][2]];
  } else {
    pal[0] = kCgaRgb565[0];                                                    // mono: 0 = black
    int fg = ga->graphForegroundIndex() & 0x0F;
    pal[1] = kCgaRgb565[fg ? fg : 15];                                         // 1 = fg colour (def. white)
  }

  displaySetUiMode(false);
  displaySetVideoRect(20, 200);         // 200 active lines centered in 240
  displaySetVideoFill(0, 320, true);    // full-width, stretched to the panel
  tft.setSwapBytes(true);
  for (int oy = 0; oy < 200; ) {
    int n = 0;
    while (oy + n < 200 && n < 8) {
      int sy = oy + n;
      const uint8_t* srow = vm + (sy & 1) * 0x2000 + (sy >> 1) * 80;
      uint16_t* dst = pcGfxScratch + n * 320;
      if (!mode640) {
        for (int x = 0; x < 320; x++)
          dst[x] = pal[(srow[x >> 2] >> (6 - (x & 3) * 2)) & 3];
      } else {
        for (int x = 0; x < 320; x++) {                  // 640->320: OR pixel pairs so thin lines survive
          int sx = x * 2;
          uint8_t p0 = (srow[sx >> 3]       >> (7 - (sx & 7)))       & 1;
          uint8_t p1 = (srow[(sx + 1) >> 3] >> (7 - ((sx + 1) & 7))) & 1;
          dst[x] = pal[(p0 | p1) ? 1 : 0];
        }
      }
      n++;
    }
    tft.pushImage(0, 20 + oy, 320, n, pcGfxScratch);
    oy += n;
  }
  tft.setSwapBytes(false);
}

// ---- CGA text render: 80x25 / 40x25 with the built-in 6x8 font (like iigsRenderText) ----
static void pcxtRenderText() {
  tft.setUiMode(true);
  bool osk = oskActive();
  int kbdTop = osk ? oskRasterHeight() : 240;

  bool text80 = (g_pcxtMachine.graphicsAdapter()->emulation()
                 == fabgl::GraphicsAdapter::Emulation::PC_Text_80x25_16Colors);
  int cols = text80 ? 80 : 40;

  const uint8_t* vbuf = g_pcxtMachine.videoMemory() + 0x8000 + g_pcxtMachine.cgaMemOffset();

  if (osk) tft.fillRect(0, 0, 320, kbdTop, TFT_BLACK);
  else     tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);

  // Each glyph advances 6 NATIVE px = 4 logical units (drawString draws unscaled glyphs). So a column
  // is logical X0 + c*4, and the text block is cols*4 logical wide (80->320 full width, 40->160 centered).
  const int LCW = 4;                          // logical units per char cell (= 6 native px)
  int X0 = text80 ? 0 : (320 - cols * LCW) / 2;
  float rowH = (kbdTop - 1.0f) / 25.0f;
  char line[81];
  // Pass 1: fill every row's background band, with +1px overlap so the logical->native (x272/240)
  // rounding never leaves a black stripe between rows. (One bg per row = first cell's attribute;
  // per-char colour comes with the M3 renderer.) Fills go first so they don't clip the text below.
  for (int r = 0; r < 25; r++) {
    uint8_t attr = vbuf[(r * cols) * 2 + 1];
    int yTop = (int)(r * rowH + 0.5f);
    int yBot = (int)((r + 1) * rowH + 0.5f);
    tft.fillRect(X0, yTop, cols * LCW, (yBot - yTop) + 1, kCgaRgb565[(attr >> 4) & 0x07]);
  }
  // Pass 2: draw text (transparent fg) over the completed background.
  for (int r = 0; r < 25; r++) {
    uint8_t attr = vbuf[(r * cols) * 2 + 1];
    tft.setTextColor(kCgaRgb565[attr & 0x0F]);
    for (int c = 0; c < cols; c++) {
      uint8_t ch = vbuf[(r * cols + c) * 2];
      line[c] = (ch < 0x20 || ch > 0x7E) ? ' ' : (char)ch;
    }
    line[cols] = 0;
    tft.drawString(line, X0, (int)(r * rowH + 0.5f), 1);
  }

  // hardware cursor (6845): blinking underline ('_') at the cursor cell.
  // drawString maps the START (x,y) logical->native (x1.5) but advances each glyph by 6 NATIVE px,
  // so column cc lands at logical X0 + cc*(6*320/480) = X0 + cc*4. Drawing the cursor as a glyph (not
  // a logical fillRect, which would be x1.5 too wide and drift right) keeps it aligned + sized like the text.
  fabgl::GraphicsAdapter* ga = g_pcxtMachine.graphicsAdapter();
  if (ga->cursorVisible() && ((millis() / 400) & 1) == 0) {
    int cr = ga->cursorRow(), cc = ga->cursorCol();
    if (cr >= 0 && cr < 25 && cc >= 0 && cc < cols) {
      tft.setTextColor(kCgaRgb565[15]);                       // bright, transparent overlay (no bg)
      tft.drawString("_", X0 + cc * LCW, (int)(cr * rowH + 0.5f), 1);
    }
  }
}

// Dispatch by CGA mode (text vs graphics). Clears the panel on a mode switch so no remnants linger.
void pcxtRenderFrame() {
  if (!pcInitDone) return;
  auto emu = g_pcxtMachine.graphicsAdapter()->emulation();
  bool gfx = (emu == fabgl::GraphicsAdapter::Emulation::PC_Graphics_320x200_4Colors ||
              emu == fabgl::GraphicsAdapter::Emulation::PC_Graphics_640x200_2Colors);
  static int lastGfx = -1;
  if ((int)gfx != lastGfx) {                 // text <-> graphics switch: wipe canvas + panel border
    lastGfx = (int)gfx;
    tft.setUiMode(true);
    tft.fillScreen(TFT_BLACK);
#if BOARD_DISPLAY_GFX
    tft.fillPanelBlack();
#endif
  }
  if (gfx) pcxtRenderGraphics(emu);
  else     pcxtRenderText();
}

bool pcxtRenderLoadWarning() {
  // Always run the BIOS (it shows POST + a "no boot disk" message itself), so we
  // never block the screen. Returning false lets pcxtRenderFrame draw the CGA text.
  return false;
}

// ---- input: USB HID usage -> XT (set-1) scancode -> i8042 ----
// Returns set-1 make code (break = code|0x80). *e0 set for E0-prefixed extended keys.
static uint8_t hidToScan(uint8_t usage, bool* e0) {
  *e0 = false;
  if (usage >= 0x04 && usage <= 0x1D) {     // A..Z (HID order) -> set-1 letter codes
    static const uint8_t L[26] = {
      0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
      0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
    return L[usage - 0x04];
  }
  if (usage >= 0x1E && usage <= 0x26) return (uint8_t)(0x02 + (usage - 0x1E));  // 1..9
  if (usage == 0x27) return 0x0B;           // 0
  switch (usage) {
    case 0x28: return 0x1C;  // Enter
    case 0x29: return 0x01;  // Esc
    case 0x2A: return 0x0E;  // Backspace
    case 0x2B: return 0x0F;  // Tab
    case 0x2C: return 0x39;  // Space
    case 0x2D: return 0x0C;  // -
    case 0x2E: return 0x0D;  // =
    case 0x2F: return 0x1A;  // [
    case 0x30: return 0x1B;  // ]
    case 0x31: return 0x2B;  // backslash
    case 0x33: return 0x27;  // ;
    case 0x34: return 0x28;  // '
    case 0x35: return 0x29;  // `
    case 0x36: return 0x33;  // ,
    case 0x37: return 0x34;  // .
    case 0x38: return 0x35;  // /
    case 0x39: return 0x3A;  // CapsLock
    case 0x3A: return 0x3B;  case 0x3B: return 0x3C;  case 0x3C: return 0x3D;  // F1..F3
    case 0x3D: return 0x3E;  case 0x3E: return 0x3F;  case 0x3F: return 0x40;  // F4..F6
    case 0x40: return 0x41;  case 0x41: return 0x42;  case 0x42: return 0x43;  // F7..F9
    case 0x43: return 0x44;  case 0x44: return 0x57;  case 0x45: return 0x58;  // F10..F12
    case 0x4F: *e0 = true; return 0x4D;  // Right
    case 0x50: *e0 = true; return 0x4B;  // Left
    case 0x51: *e0 = true; return 0x50;  // Down
    case 0x52: *e0 = true; return 0x48;  // Up
    case 0xE0: return 0x1D;  // LCtrl
    case 0xE1: return 0x2A;  // LShift
    case 0xE2: return 0x38;  // LAlt
    case 0xE4: *e0 = true; return 0x1D;  // RCtrl
    case 0xE5: return 0x36;  // RShift
    default:   return 0x00;
  }
}

void pcxtKeyDown(uint8_t hidUsage, bool /*shift*/, bool /*ctrl*/, bool /*alt*/) {
  bool e0; uint8_t sc = hidToScan(hidUsage, &e0);
  if (!sc) return;
  if (e0) g_pcxtMachine.injectScancode(0xE0);
  g_pcxtMachine.injectScancode(sc);
}

void pcxtKeyUp(uint8_t hidUsage) {
  bool e0; uint8_t sc = hidToScan(hidUsage, &e0);
  if (!sc) return;
  if (e0) g_pcxtMachine.injectScancode(0xE0);
  g_pcxtMachine.injectScancode(sc | 0x80);
}

// gamepad -> arrow keys + Enter/Esc (active-low mask: b0 up,b1 down,b2 left,b3 right,b4 A,b5 B)
void pcxtSetInput(uint8_t joyMask) {
  static uint8_t prev = 0xFF;
  struct { uint8_t bit; uint8_t usage; } map[] = {
    {0x01, 0x52}, {0x02, 0x51}, {0x04, 0x50}, {0x08, 0x4F}, {0x10, 0x28}, {0x20, 0x29} };
  for (auto& m : map) {
    bool nowDown  = !(joyMask & m.bit);
    bool prevDown = !(prev   & m.bit);
    if (nowDown && !prevDown) pcxtKeyDown(m.usage, false, false, false);
    else if (!nowDown && prevDown) pcxtKeyUp(m.usage);
  }
  prev = joyMask;
}

void pcxtHardReset() { pcResetReq = true; }

// ---- settings hooks ----
void pcxtScanFiles() { loadPcxtFilesSync(); }

// Mount the selected image explicitly into A: (floppy) or C: (hard disk), from the settings menu.
// No reboot: an A: change is seen live by DOS; a new C: needs a reboot to be recognised.
bool pcxtMountA(const char* path) { return pcMountInto(path, 0); }
bool pcxtMountC(const char* path) { return pcMountInto(path, 2); }

// Eject the disk in a slot (0 = A:, 2 = C:): close the image and clear the saved name.
void pcxtUnmount(int slot) {
  if (!pcInitDone) return;
  g_pcxtMachine.setDriveImage(slot, nullptr);      // null filename -> close + leave the slot empty
  if (slot == 0) selectedPcFileName = ""; else selectedPcHdFileName = "";
  g_pcxtMachine.setBootDrive(g_pcxtMachine.disk(0) ? 0 : 2);
  printLog(slot == 0 ? "PCXT: ejected A:" : "PCXT: ejected C:");
}
