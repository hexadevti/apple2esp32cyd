// ui_imgui.cpp — desktop native UI shell (see ui_imgui.h). Dear ImGui (docking) over SDL2 +
// SDL_Renderer. Presents the emulator framebuffer as an aspect-fit, dockable image and hosts the
// menu bar / settings / debug panels. Desktop-only (BOARD_DESKTOP); never compiled for the device.
#if defined(BOARD_DESKTOP)

#include "ui_imgui.h"
#include "debug_bridge.h"            // uniform debug facade over the cores (regs / memory / exec control)
#include "imgui.h"
#include "imgui_internal.h"          // DockBuilder* for the one-time default layout
#include "imgui_memory_editor.h"     // vendored hex viewer/editor (imgui_club)
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include <SDL.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cctype>

// The active emulator-video content rect inside the 320x240 framebuffer (cores set it each frame via
// displaySetVideoRect/Fill). "Crop borders" samples just this sub-rect. Defined in display_sdl.cpp.
void desktopGetVideoRect(int *l, int *t, int *w, int *h);

extern bool running;     // globals.cpp — cleared on Quit (kept light; we don't pull in all of emu.h here)

// Settings globals (emu.h) the native Settings window flips live; saveConfig persists them.
extern bool sound, videoColor, smoothUpscale, screenFill, Fast1MhzSpeed;
void        saveConfig();
const char *desktopSdRoot();              // sd_host.cpp — host dir backing the emulated SD (file browser)
void        desktopGetCurrentWindowSize(int *w, int *h);   // display_sdl.cpp — live SDL window size
int         desktopAudioRate();                            // audio_sdl.cpp — output sample rate
int         desktopAudioSnapshot(float *out, int n);       // audio_sdl.cpp — most-recent N output samples
const char *desktopBaseDir();                              // hal.cpp — directory of the .exe (trailing slash)

static int g_cfgWinW = 0, g_cfgWinH = 0;  // window size loaded from emu6502.cfg (0 = none stored)

// Persistence files anchored next to the .exe (NOT cwd) so a saved session is found regardless of
// where the app is launched from. Resolved once, before the window is created.
static std::string g_cfgPath, g_iniPath;
static void resolvePersistPaths() {
  if (!g_cfgPath.empty()) return;
  std::string base = desktopBaseDir();
  g_cfgPath = base + "emu6502.cfg";
  g_iniPath = base + "imgui.ini";
}

// --- shared SDL handles (owned by display_sdl.cpp) ---
static SDL_Window   *g_win = nullptr;
static SDL_Renderer *g_ren = nullptr;

// --- view options (wired to the View menu) ---
static bool g_filterLinear = true;    // emulator image scaling: linear (smooth) vs nearest (crisp pixels)
static bool g_cropToVideo  = false;   // show only the active video rect (drop the cores' black borders)
static bool g_showDemo     = false;   // ImGui demo window (dev aid)
static bool g_buildLayout  = false;   // build the default dock layout once (only when no imgui.ini yet)

// --- debug panel visibility (Debug menu) ---
static bool g_showCtrl   = true;      // execution control (pause/step/reset)
static bool g_showCpu    = true;      // CPU register/flag state
static bool g_showMem    = true;      // memory hex viewer/editor
static bool g_showDisasm = true;      // disassembly + breakpoints
static bool g_showHeat   = false;     // memory-access heat map
static bool g_showDisk   = false;     // disk-read heat map (Apple II)
static bool g_showSpectrum = false;   // audio spectrum analyzer
static bool g_showIo     = true;      // I/O / soft-switch state
static bool g_disasmFollow = true;    // disassembly: keep PC in view (persisted)
static bool g_showBp     = false;     // breakpoints + watchpoints manager
static bool g_showLoad     = false;   // SD file browser (load disk/cart)
static bool g_showSettings = false;   // native settings window

// heat-map render texture (256x256, one pixel per CPU byte; row = 256-byte page)
static SDL_Texture *g_heatTex = nullptr;
static bool         g_heatFade = false;
static float        g_heatZoom = 1.0f;   // mouse-wheel zoom of the memory map
static bool         g_diskFade = false;  // disk heat map: fade old accesses

// Hex viewer over the debug facade: ReadFn/WriteFn route to the side-effect-free peek/poke so the
// viewer never trips emulator soft-switches. mem_data is unused (the callbacks ignore it).
static MemoryEditor g_memEdit;
static ImU8  memRead(const ImU8 *, size_t off, void *)        { return dbgPeek((uint32_t)off); }
static void  memWrite(ImU8 *, size_t off, ImU8 d, void *)     { dbgPoke((uint32_t)off, d); }

// --- last emulator-image placement, for window-pixel -> framebuffer mouse mapping ---
static SDL_Rect g_srcRect = {0, 0, 0, 0};   // sub-rect of the framebuffer shown (fb coords)
static SDL_Rect g_dstRect = {0, 0, 0, 0};   // where it landed on screen (window pixels)

// --- audio spectrum analyzer --------------------------------------------------------------------
// In-place iterative radix-2 FFT (n a power of two).
static void fftRadix2(float *re, float *im, int n)
{
  for (int i = 1, j = 0; i < n; i++) {              // bit-reversal permutation
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * (float)M_PI / (float)len;
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1.0f, ci = 0.0f;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k, b = a + len / 2;
        float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr;        im[a] += ti;
        float ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
      }
    }
  }
}

static float g_specBars[96] = {0};   // smoothed display bars (persist across frames for a smooth decay)

static void buildSpectrumPanel()
{
  if (!g_showSpectrum) return;
  if (ImGui::Begin("Audio spectrum", &g_showSpectrum)) {
    const int N = 1024, BARS = 96;
    static float re[N], im[N], mag[N / 2];
    desktopAudioSnapshot(re, N);
    for (int i = 0; i < N; i++) { float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (N - 1))); re[i] *= w; im[i] = 0; }
    fftRadix2(re, im, N);
    int rate = desktopAudioRate(); if (rate <= 0) rate = 44100;
    float mx = 0.02f;                                // floor so silence stays flat
    for (int i = 0; i < N / 2; i++) { mag[i] = sqrtf(re[i] * re[i] + im[i] * im[i]); if (mag[i] > mx) mx = mag[i]; }

    ImGui::TextDisabled("%d Hz   FFT %d   log-frequency 40 Hz..%g kHz", rate, N, rate * 0.0005);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.y < 60) avail.y = 60;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + avail.x, p0.y + avail.y), IM_COL32(16, 16, 22, 255));
    float fmin = 40.0f, fmax = (float)rate * 0.5f, bw = avail.x / BARS;
    for (int b = 0; b < BARS; b++) {
      float f0 = fmin * powf(fmax / fmin, (float)b / BARS);
      float f1 = fmin * powf(fmax / fmin, (float)(b + 1) / BARS);
      int i0 = (int)(f0 * N / rate), i1 = (int)(f1 * N / rate);
      if (i0 < 1) i0 = 1; if (i1 <= i0) i1 = i0 + 1; if (i1 > N / 2) i1 = N / 2;
      float peak = 0; for (int i = i0; i < i1; i++) if (mag[i] > peak) peak = mag[i];
      float v = sqrtf(peak / mx);                    // 0..1
      g_specBars[b] = (v > g_specBars[b]) ? v : g_specBars[b] * 0.80f + v * 0.20f;   // fast attack, smooth decay
      float h = g_specBars[b] * avail.y;
      ImU32 col = IM_COL32((int)(70 + 185 * g_specBars[b]), (int)(225 - 150 * g_specBars[b]), 70, 255);
      ImVec2 a(p0.x + b * bw, p0.y + avail.y - h);
      dl->AddRectFilled(a, ImVec2(a.x + bw - 1.0f, p0.y + avail.y), col);
    }
    ImGui::Dummy(avail);
  }
  ImGui::End();
}

// --- session persistence (emu6502.cfg = window size + view prefs + which panels are open) ---------
void desktopUiLoadConfig()
{
  resolvePersistPaths();
  FILE *f = fopen(g_cfgPath.c_str(), "r");
  if (!f) return;
  char line[160];
  while (fgets(line, sizeof(line), f)) {
    char key[64]; int v;
    if (sscanf(line, "%63[^=]=%d", key, &v) != 2) continue;
    #define CFG(name, var) else if (strcmp(key, name) == 0) var = (v != 0)
    if (strcmp(key, "winW") == 0) g_cfgWinW = v;
    else if (strcmp(key, "winH") == 0) g_cfgWinH = v;
    else if (strcmp(key, "memCols") == 0) g_memEdit.Cols = (v >= 4 && v <= 64) ? v : 16;
    CFG("filterLinear", g_filterLinear); CFG("cropToVideo", g_cropToVideo);
    CFG("showCtrl", g_showCtrl);   CFG("showCpu", g_showCpu);   CFG("showIo", g_showIo);
    CFG("showDisasm", g_showDisasm); CFG("showBp", g_showBp);   CFG("showMem", g_showMem);
    CFG("showHeat", g_showHeat);   CFG("showDisk", g_showDisk);
    CFG("showSettings", g_showSettings); CFG("showSpectrum", g_showSpectrum);
    CFG("heatFade", g_heatFade);   CFG("disasmFollow", g_disasmFollow); CFG("diskFade", g_diskFade);
    CFG("memAscii", g_memEdit.OptShowAscii);   CFG("memOpts", g_memEdit.OptShowOptions);
    CFG("memGrey", g_memEdit.OptGreyOutZeroes); CFG("memPreview", g_memEdit.OptShowDataPreview);
    else if (strcmp(key, "heatRecord") == 0) { if (v) dbgHeatEnable(true); }      // restore Record state
    else if (strcmp(key, "diskRecord") == 0) { if (v) dbgDiskHeatEnable(true); }
    else if (strcmp(key, "clockMilliMhz") == 0) dbgSetClockMhz((float)v / 1000.0f); // restore clock speed
    #undef CFG
  }
  fclose(f);
}
void desktopUiGetWindowSize(int *w, int *h) { if (w) *w = g_cfgWinW; if (h) *h = g_cfgWinH; }

// Write JUST emu6502.cfg (window size + view prefs + open panels + per-panel options). Cheap; called
// both on a debounced auto-save (so options persist WITHOUT a clean quit, like ImGui's own ini) and
// from desktopUiSaveState() on exit.
static void desktopWriteCfg()
{
  int w = 0, h = 0; desktopGetCurrentWindowSize(&w, &h);
  resolvePersistPaths();
  FILE *f = fopen(g_cfgPath.c_str(), "w");
  if (!f) return;
  if (w > 0 && h > 0) fprintf(f, "winW=%d\nwinH=%d\n", w, h);
  fprintf(f, "filterLinear=%d\ncropToVideo=%d\n", g_filterLinear, g_cropToVideo);
  fprintf(f, "showCtrl=%d\nshowCpu=%d\nshowIo=%d\nshowDisasm=%d\nshowBp=%d\nshowMem=%d\nshowHeat=%d\nshowDisk=%d\n",
          g_showCtrl, g_showCpu, g_showIo, g_showDisasm, g_showBp, g_showMem, g_showHeat, g_showDisk);
  fprintf(f, "showSettings=%d\nshowSpectrum=%d\nheatFade=%d\ndisasmFollow=%d\n",
          g_showSettings, g_showSpectrum, g_heatFade, g_disasmFollow);
  fprintf(f, "memCols=%d\nmemAscii=%d\nmemOpts=%d\nmemGrey=%d\nmemPreview=%d\n",
          g_memEdit.Cols, g_memEdit.OptShowAscii, g_memEdit.OptShowOptions,
          g_memEdit.OptGreyOutZeroes, g_memEdit.OptShowDataPreview);
  fprintf(f, "heatRecord=%d\ndiskRecord=%d\ndiskFade=%d\n", dbgHeatEnabled(), dbgDiskHeatEnabled(), g_diskFade);
  fprintf(f, "clockMilliMhz=%d\n", (int)(dbgGetClockMhz() * 1000.0f + 0.5f));
  fclose(f);
}

// Re-write emu6502.cfg whenever any persisted value changed, debounced ~0.7 s (so a resize-drag or a
// burst of toggles writes once after it settles). Runs every frame; cheap (a snprintf + strcmp).
static void desktopUiAutoSaveCfg()
{
  int w = 0, h = 0; desktopGetCurrentWindowSize(&w, &h);
  char sig[256];
  snprintf(sig, sizeof(sig), "%d,%d,%d,%d|%d%d%d%d%d%d%d%d%d%d|%d%d|%d,%d%d%d%d|%d%d",
           w, h, g_filterLinear, g_cropToVideo,
           g_showCtrl, g_showCpu, g_showIo, g_showDisasm, g_showBp, g_showMem, g_showHeat, g_showDisk,
           g_showSettings, g_showSpectrum, g_heatFade, g_disasmFollow,
           g_memEdit.Cols, (int)g_memEdit.OptShowAscii, (int)g_memEdit.OptShowOptions,
           (int)g_memEdit.OptGreyOutZeroes, (int)g_memEdit.OptShowDataPreview,
           (int)dbgHeatEnabled(), (int)dbgDiskHeatEnabled());
  char sig2[32]; snprintf(sig2, sizeof(sig2), "|c%d|%d", (int)(dbgGetClockMhz() * 1000.0f), (int)g_diskFade);
  strncat(sig, sig2, sizeof(sig) - strlen(sig) - 1);
  static char last[256] = {0};
  static int  countdown = -1;
  if (strcmp(sig, last) != 0) { strncpy(last, sig, sizeof(last) - 1); countdown = 45; }   // change -> arm
  if (countdown > 0 && --countdown == 0) desktopWriteCfg();                                // settled -> write
}

void desktopUiSaveState()
{
  desktopWriteCfg();
  if (ImGui::GetCurrentContext()) ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);  // dock layout
  saveConfig();   // persist emulator settings + last-loaded disk to eeprom.bin
}

void desktopUiInit(SDL_Window *win, SDL_Renderer *ren)
{
  g_win = win;
  g_ren = ren;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;       // dockable debug panels
  // NOTE: do NOT enable NavEnableKeyboard — it makes ImGui hold WantCaptureKeyboard whenever a panel
  // is focused, which would steal the keyboard from the emulator. The emulator needs every key; we
  // only divert the keyboard to ImGui while an actual text field is being edited (WantTextInput).
  io.FontGlobalScale = 1.25f;                             // comfortable, crisp at desktop sizes

  resolvePersistPaths();
  io.IniFilename = g_iniPath.c_str();                     // dock layout next to the .exe (g_iniPath outlives io)

  // Build the default dock layout only on a fresh install (no imgui.ini to restore). After that the
  // user's saved layout wins.
  if (FILE *f = fopen(g_iniPath.c_str(), "rb")) { fclose(f); g_buildLayout = false; }
  else                                          { g_buildLayout = true; }

  ImGui::StyleColorsDark();
  ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
  ImGui_ImplSDLRenderer2_Init(ren);

  g_memEdit.ReadFn  = memRead;         // hex viewer reads/writes through the side-effect-free facade
  g_memEdit.WriteFn = memWrite;

  if (getenv("EMU_DBG_PAUSE")) dbgSetPaused(true);   // boot paused (handy for offline debug captures)
  if (getenv("EMU_DBG_HEAT")) { g_showHeat = true; g_showMem = false; dbgHeatEnable(true); }  // heat capture
  if (getenv("EMU_DBG_DIS"))  { g_showMem = false; g_showHeat = false; }  // disasm capture (sole bottom tab)
  if (getenv("EMU_DBG_LOAD"))     g_showLoad = true;        // open the file browser (offline capture)
  if (getenv("EMU_DBG_SETTINGS")) g_showSettings = true;    // open settings (offline capture)
  if (getenv("EMU_DBG_IO"))       g_showCpu = false;        // I/O panel solo in its tab (offline capture)
  if (getenv("EMU_DBG_DISK")) { g_showDisk = true; g_showMem = g_showHeat = g_showDisasm = false; dbgDiskHeatEnable(true); }
  if (getenv("EMU_DBG_SPECTRUM")) { g_showSpectrum = true; g_showMem = g_showHeat = g_showDisasm = g_showDisk = false; }
  if (const char *c = getenv("EMU_DBG_CLOCK")) { dbgSetThrottle(true); dbgSetClockMhz((float)atof(c)); }  // clock self-test
  if (const char *w = getenv("EMU_DBG_WATCH")) {           // set a R+W watchpoint at boot (self-test)
    g_showBp = true; g_showDisasm = false;
    dbgWatchToggle((uint32_t)strtol(w, nullptr, 16), WATCH_R | WATCH_W);
  }
}

void desktopUiShutdown()
{
  if (!ImGui::GetCurrentContext()) return;
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
}

void desktopUiProcessEvent(const SDL_Event *e)
{
  if (ImGui::GetCurrentContext()) ImGui_ImplSDL2_ProcessEvent(e);
}

bool desktopUiWantCaptureMouse()    { return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse; }
// Only divert the keyboard from the emulator while an ImGui TEXT FIELD is being edited (goto/filter).
// Using WantCaptureKeyboard here instead would let any focused panel swallow the emulator's keys.
bool desktopUiWantCaptureKeyboard() { return ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput; }

bool desktopUiMapToEmu(int winX, int winY, int *ox, int *oy)
{
  if (g_dstRect.w <= 0 || g_dstRect.h <= 0) return false;
  if (winX < g_dstRect.x || winX >= g_dstRect.x + g_dstRect.w ||
      winY < g_dstRect.y || winY >= g_dstRect.y + g_dstRect.h) return false;
  float fx = (winX - g_dstRect.x) / (float)g_dstRect.w;
  float fy = (winY - g_dstRect.y) / (float)g_dstRect.h;
  int x = g_srcRect.x + (int)(fx * g_srcRect.w);
  int y = g_srcRect.y + (int)(fy * g_srcRect.h);
  if (ox) *ox = x;
  if (oy) *oy = y;
  return true;
}

// --- menu bar -------------------------------------------------------------------------------------
static void buildMenuBar()
{
  if (!ImGui::BeginMainMenuBar()) return;

  if (ImGui::BeginMenu("System")) {
    if (ImGui::BeginMenu("Platform")) {
      int cur = dbgPlatform();
      for (int p = 0; p < dbgPlatformCount(); p++)
        if (ImGui::MenuItem(dbgPlatformName(p), nullptr, p == cur)) dbgSwitchPlatform(p);  // reboots into p
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Load disk / cartridge...")) g_showLoad = true;
    ImGui::MenuItem("Settings", nullptr, &g_showSettings);
    ImGui::Separator();
    bool p = dbgIsPaused();
    if (ImGui::MenuItem(p ? "Resume" : "Pause", "F5")) dbgSetPaused(!p);
    if (ImGui::MenuItem("Step", "F10", false, p && dbgStepSupported())) dbgStep();
    ImGui::Separator();
    if (ImGui::MenuItem("Reset (reboot)")) dbgReset();
    if (ImGui::MenuItem("Quit")) { desktopUiSaveState(); running = false; SDL_Quit(); std::exit(0); }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem("Linear filter (smooth)", nullptr, &g_filterLinear);
    ImGui::MenuItem("Crop borders (active video only)", nullptr, &g_cropToVideo);
    ImGui::Separator();
    ImGui::MenuItem("ImGui demo window", nullptr, &g_showDemo);
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Debug")) {
    ImGui::MenuItem("Execution control", nullptr, &g_showCtrl);
    ImGui::MenuItem("CPU state", nullptr, &g_showCpu);
    ImGui::MenuItem("Disassembly", nullptr, &g_showDisasm);
    ImGui::MenuItem("Breakpoints / watchpoints", nullptr, &g_showBp);
    ImGui::MenuItem("Memory", nullptr, &g_showMem);
    ImGui::MenuItem("I/O (soft switches)", nullptr, &g_showIo);
    ImGui::MenuItem("Heat map (memory)", nullptr, &g_showHeat);
    ImGui::MenuItem("Heat map (disk reads)", nullptr, &g_showDisk);
    ImGui::MenuItem("Audio spectrum", nullptr, &g_showSpectrum);
    ImGui::EndMenu();
  }

  // right-aligned state + FPS readout
  char info[64];
  snprintf(info, sizeof(info), "%s   %.0f FPS", dbgIsPaused() ? "PAUSED" : "RUNNING", ImGui::GetIO().Framerate);
  float w = ImGui::CalcTextSize(info).x;
  ImGui::SameLine(ImGui::GetWindowWidth() - w - 16.0f);
  ImGui::TextColored(dbgIsPaused() ? ImVec4(1.0f, 0.7f, 0.2f, 1.0f) : ImVec4(0.5f, 0.8f, 0.5f, 1.0f), "%s", info);

  ImGui::EndMainMenuBar();
}

// --- debug panels ---------------------------------------------------------------------------------
static void buildControlPanel()
{
  if (!g_showCtrl) return;
  if (ImGui::Begin("Control", &g_showCtrl)) {
    bool p = dbgIsPaused();
    if (p) { if (ImGui::Button("Resume", ImVec2(80, 0))) dbgSetPaused(false); }
    else   { if (ImGui::Button("Pause",  ImVec2(80, 0))) dbgSetPaused(true);  }
    ImGui::SameLine();
    ImGui::BeginDisabled(!(p && dbgStepSupported()));
    if (ImGui::Button("Step", ImVec2(80, 0))) dbgStep();            // step into
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reboot", ImVec2(80, 0))) dbgReset();          // re-exec (loses session)

    // second row: subroutine-aware stepping + a session-preserving soft reset (6502)
    ImGui::BeginDisabled(!(p && dbgRunControlSupported()));
    if (ImGui::Button("Step Over", ImVec2(80, 0))) dbgStepOver();
    ImGui::SameLine();
    if (ImGui::Button("Step Out", ImVec2(80, 0))) dbgStepOut();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!dbgSoftResetSupported());
    if (ImGui::Button("Soft Reset", ImVec2(80, 0))) dbgSoftReset();
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Text("State: %s", p ? "PAUSED" : "RUNNING");
    ImGui::Text("CPU:   %s", dbgCpuName());
    if (dbgMemReadable()) ImGui::Text("PC:    $%04X", dbgGetPC());
    if (!dbgStepSupported())
      ImGui::TextDisabled("(single-step not wired for this platform)");

    // clock-speed controller
    if (dbgClockSupported()) {
      ImGui::SeparatorText("Clock speed");
      bool thr = dbgGetThrottle();
      if (ImGui::Checkbox("Throttle", &thr)) dbgSetThrottle(thr);
      ImGui::SameLine(); ImGui::TextDisabled(thr ? "(paced)" : "(uncapped / Fast)");
      ImGui::BeginDisabled(!thr);
      float mhz = dbgGetClockMhz();
      ImGui::SetNextItemWidth(150);
      if (ImGui::SliderFloat("##mhz", &mhz, 0.25f, 8.0f, "%.2f MHz")) dbgSetClockMhz(mhz);
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Default speed")) { dbgSetThrottle(true); dbgSetClockMhz(dbgClockDefaultMhz()); }
      ImGui::Text("measured: %.2f MHz", dbgGetMeasuredMhz());
    }
  }
  ImGui::End();
}

static void buildCpuPanel()
{
  if (!g_showCpu) return;
  if (ImGui::Begin("CPU", &g_showCpu)) {
    ImGui::TextDisabled("%s", dbgCpuName());
    DbgReg regs[16];
    int n = dbgGetRegs(regs, 16);
    int spVal = -1;
    if (n == 0) {
      ImGui::TextDisabled("(registers not wired for this platform yet)");
    } else if (ImGui::BeginTable("regs", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
      for (int i = 0; i < n; i++) {
        if (strcmp(regs[i].name, "SP") == 0) spVal = (int)regs[i].value;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(regs[i].name);
        ImGui::TableNextColumn();
        int digits = regs[i].bits / 4;
        ImGui::Text("$%0*X", digits, regs[i].value);
      }
      ImGui::EndTable();
    }

    // status flags: uppercase = set, dim lowercase = clear
    const char *const *labels; uint32_t fv; int fc;
    if (dbgGetFlags(&labels, &fv, &fc)) {
      ImGui::Spacing();
      ImGui::TextUnformatted("Flags:");
      for (int i = 0; i < fc; i++) {
        bool set = (fv >> (fc - 1 - i)) & 1;
        ImGui::SameLine();
        ImGui::TextColored(set ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(0.45f, 0.45f, 0.45f, 1.0f),
                           "%s", labels[i]);
      }
    }

    // 6502 stack: the bytes most-recently pushed (top of $0100+SP .. $01FF), newest first
    if (spVal >= 0x0100 && dbgMemReadable()) {
      uint16_t sp = (uint16_t)spVal;
      ImGui::SeparatorText("Stack");
      ImGui::BeginChild("stk", ImVec2(0, 110));
      if (sp >= 0x01FF) ImGui::TextDisabled("(empty)");
      else for (uint16_t a = sp + 1; a <= 0x01FF; a++) ImGui::Text("$%04X: $%02X", a, dbgPeek(a));
      ImGui::EndChild();
    }
  }
  ImGui::End();
}

static void buildDisasmPanel()
{
  if (!g_showDisasm) return;
  if (ImGui::Begin("Disassembly", &g_showDisasm)) {
    static bool focusOnce = getenv("EMU_DBG_PAUSE") != nullptr;   // offline captures: bring disasm to front
    if (focusOnce) { ImGui::SetWindowFocus(); focusOnce = false; }
    if (!dbgDisasmSupported()) {
      ImGui::TextDisabled("(disassembly not wired for this platform yet)");
      ImGui::End();
      return;
    }
    static uint16_t viewAddr = 0;
    static char gotoBuf[8] = "";
    uint16_t pc = (uint16_t)dbgGetPC();

    ImGui::Checkbox("Follow PC", &g_disasmFollow);
    ImGui::SameLine(); ImGui::TextDisabled("goto");
    ImGui::SameLine(); ImGui::SetNextItemWidth(56);
    if (ImGui::InputText("##goto", gotoBuf, sizeof(gotoBuf),
                         ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
      viewAddr = (uint16_t)strtol(gotoBuf, nullptr, 16); g_disasmFollow = false;
    }
    if (dbgBpSupported()) { ImGui::SameLine(); if (ImGui::Button("Clear BPs")) dbgBpClearAll(); }
    if (g_disasmFollow) viewAddr = pc;

    ImGui::Separator();
    if (ImGui::BeginChild("disasm_lines")) {
      uint16_t a = viewAddr;
      for (int i = 0; i < 96; i++) {
        char dis[40]; int len = dbgDisasm(a, dis, sizeof(dis));
        char bytes[16] = "";
        for (int k = 0; k < len; k++) { char t[4]; snprintf(t, sizeof(t), "%02X ", dbgPeek((uint16_t)(a + k))); strcat(bytes, t); }
        bool isPC = (a == pc);
        bool bp   = dbgBpSupported() && dbgBpAt(a);
        char line[80];
        snprintf(line, sizeof(line), "%s %s%04X  %-9s %s", bp ? "*" : " ", isPC ? ">" : " ", a, bytes, dis);
        ImGui::PushID(a);
        ImGui::PushStyleColor(ImGuiCol_Text, isPC ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) : ImVec4(0.82f, 0.82f, 0.82f, 1.0f));
        bool clicked = ImGui::Selectable(line, bp);
        ImGui::PopStyleColor();
        if (clicked && dbgBpSupported()) dbgBpToggle(a);                        // left-click toggles a breakpoint
        if (ImGui::BeginPopupContextItem()) {                                   // right-click menu
          if (dbgRunControlSupported() && ImGui::MenuItem("Run to here")) dbgRunTo(a);
          if (dbgBpSupported() && ImGui::MenuItem(bp ? "Remove breakpoint" : "Add breakpoint")) dbgBpToggle(a);
          ImGui::EndPopup();
        }
        ImGui::PopID();
        a = (uint16_t)(a + len);
      }
    }
    ImGui::EndChild();
  }
  ImGui::End();
}

static void buildMemoryPanel()
{
  if (!g_showMem) return;
  if (ImGui::Begin("Memory", &g_showMem)) {
    if (dbgMemReadable()) {
      g_memEdit.WriteFn = dbgPokeSupported() ? memWrite : nullptr;   // read-only where poke is unsafe
      g_memEdit.DrawContents(nullptr, (size_t)dbgMemSize(), 0);
    } else {
      ImGui::TextDisabled("(memory view not wired for this platform yet)");
    }
  }
  ImGui::End();
}

// --- I/O / soft-switch state (Apple II banking + video switches) --------------------------------
static void buildIoPanel()
{
  if (!g_showIo) return;
  if (ImGui::Begin("I/O", &g_showIo)) {
    DbgFlag fl[24];
    int n = dbgGetIoState(fl, 24);
    if (n == 0) {
      ImGui::TextDisabled("(no soft-switch state for this platform yet)");
    } else if (ImGui::BeginTable("io", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      for (int i = 0; i < n; i++) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", fl[i].label);
        ImGui::TableNextColumn();
        ImGui::TextColored(fl[i].active ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f) : ImVec4(0.78f, 0.78f, 0.78f, 1.0f),
                           "%s", fl[i].value);
      }
      ImGui::EndTable();
    }
  }
  ImGui::End();
}

// --- breakpoints + watchpoints manager ----------------------------------------------------------
static void buildBpPanel()
{
  if (!g_showBp) return;
  if (ImGui::Begin("Breakpoints", &g_showBp)) {
    ImGui::SeparatorText("Breakpoints");
    if (!dbgBpSupported()) {
      ImGui::TextDisabled("(not supported on this platform)");
    } else {
      ImGui::SameLine(ImGui::GetContentRegionAvail().x - 70);
      if (ImGui::SmallButton("Clear all##bp")) dbgBpClearAll();
      uint16_t bps[256]; int nb = dbgBpList(bps, 256);
      if (nb == 0) ImGui::TextDisabled("(none — left-click a disassembly line to add)");
      for (int i = 0; i < nb; i++) {
        ImGui::PushID(2000 + i);
        if (ImGui::SmallButton("X")) dbgBpToggle(bps[i]);
        ImGui::SameLine();
        char dis[40]; dbgDisasm(bps[i], dis, sizeof(dis));
        if (ImGui::Selectable("##bprow", false)) dbgRunTo(bps[i]);   // click row -> run to it
        ImGui::SameLine(); ImGui::Text("$%04X  %s", bps[i], dis);
        ImGui::PopID();
      }
    }

    ImGui::SeparatorText("Watchpoints");
    if (!dbgWatchSupported()) {
      ImGui::TextDisabled("(not supported on this platform)");
    } else {
      static char wbuf[8] = ""; static bool wR = true, wW = true;
      ImGui::SetNextItemWidth(56);
      ImGui::InputTextWithHint("##waddr", "addr", wbuf, sizeof(wbuf), ImGuiInputTextFlags_CharsHexadecimal);
      ImGui::SameLine(); ImGui::Checkbox("R", &wR);
      ImGui::SameLine(); ImGui::Checkbox("W", &wW);
      ImGui::SameLine();
      if (ImGui::SmallButton("Add##wp") && wbuf[0]) {
        uint16_t a = (uint16_t)strtol(wbuf, nullptr, 16);
        uint8_t want = (uint8_t)((wR ? WATCH_R : 0) | (wW ? WATCH_W : 0));
        dbgWatchToggle(a, (uint8_t)(dbgWatchAt(a) ^ want));   // set this address to exactly R/W
      }
      ImGui::SameLine(); if (ImGui::SmallButton("Clear all##wp")) dbgWatchClearAll();
      if (g_dbgWatchHit >= 0)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "last hit: $%04X", g_dbgWatchHit);
      int shown = 0;
      for (int a = 0; a < 0x10000 && shown < 64; a++) {
        uint8_t m = dbgWatchAt((uint32_t)a);
        if (!m) continue;
        shown++;
        ImGui::PushID(3000 + a);
        if (ImGui::SmallButton("X")) dbgWatchToggle((uint32_t)a, m);   // clear all bits at this addr
        ImGui::SameLine();
        ImGui::Text("$%04X  %s%s", a, (m & WATCH_R) ? "R" : "-", (m & WATCH_W) ? "W" : "-");
        ImGui::PopID();
      }
    }
  }
  ImGui::End();
}

// Memory-access heat map: 256x256 texture, 1px = 1 byte of CPU space, row = one 256-byte page.
// Green = reads, red = writes, blue = executed (opcode fetch); log-scaled so light use still shows.
static void buildHeatPanel()
{
  if (!g_showHeat) return;
  if (ImGui::Begin("Heat map", &g_showHeat)) {
    if (!dbgMemReadable()) { ImGui::TextDisabled("(no memory map for this platform yet)"); ImGui::End(); return; }
    bool on = dbgHeatEnabled();
    if (ImGui::Checkbox("Record", &on)) dbgHeatEnable(on);
    ImGui::SameLine(); if (ImGui::Button("Clear")) dbgHeatClear();
    ImGui::SameLine(); ImGui::Checkbox("Fade", &g_heatFade);
    ImGui::SameLine(); if (ImGui::Button("1x")) g_heatZoom = 1.0f;
    ImGui::SameLine(); ImGui::TextDisabled("%.0f%%", g_heatZoom * 100.0f);
    ImGui::TextDisabled("grayscale = value (00 black .. FF 50%% gray);  +green read +red write +blue exec;  wheel = zoom");

    if (!g_heatTex) {
      g_heatTex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 256, 256);
      SDL_SetTextureScaleMode(g_heatTex, SDL_ScaleModeNearest);
    }
    // build the 256x256 image: base grayscale = byte VALUE (00->black, FF->~50% gray), then add the
    // R/W/X access heat as green/red/blue when Record is on.
    bool heat = on && dbgHeatBuf(DBG_HEAT_R);
    const uint32_t *R = nullptr, *W = nullptr, *X = nullptr; float lm = 1.0f;
    if (heat) {
      if (g_heatFade) dbgHeatDecay(0.90f);
      R = dbgHeatBuf(DBG_HEAT_R); W = dbgHeatBuf(DBG_HEAT_W); X = dbgHeatBuf(DBG_HEAT_X);
      uint32_t mx = 1;
      for (int i = 0; i < 65536; i++) { if (R[i] > mx) mx = R[i]; if (W[i] > mx) mx = W[i]; if (X[i] > mx) mx = X[i]; }
      lm = logf(1.0f + (float)mx);
    }
    static uint32_t px[256 * 256];
    for (int a = 0; a < 65536; a++) {
      int base = dbgPeek((uint32_t)a) >> 1;                 // 0x00->0, 0xFF->127 (~50% gray)
      int r = base, g = base, b = base;
      if (heat) {
        g += (int)(255.0f * logf(1.0f + (float)R[a]) / lm); // read  -> green
        r += (int)(255.0f * logf(1.0f + (float)W[a]) / lm); // write -> red
        b += (int)(255.0f * logf(1.0f + (float)X[a]) / lm); // exec  -> blue
        if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
      }
      px[a] = 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;   // ABGR8888
    }
    SDL_UpdateTexture(g_heatTex, nullptr, px, 256 * sizeof(uint32_t));

    // scrollable, mouse-wheel-zoomable view (zoom centered on the cursor; nearest = crisp pixels)
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float bs = (avail.x < avail.y ? avail.x : avail.y); if (bs < 64) bs = 64;
    ImGui::BeginChild("heatview", avail, false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGuiIO &io = ImGui::GetIO();
    ImVec2 p = ImGui::GetCursorScreenPos(), mo = io.MousePos;
    float dim = bs * g_heatZoom;
    float relx = (mo.x - p.x) / dim, rely = (mo.y - p.y) / dim;
    bool hov = ImGui::IsWindowHovered();
    if (hov && io.MouseWheel != 0.0f && relx >= 0 && relx <= 1 && rely >= 0 && rely <= 1) {
      float nz = g_heatZoom * powf(1.2f, io.MouseWheel);
      if (nz < 1.0f) nz = 1.0f; else if (nz > 48.0f) nz = 48.0f;
      float nd = bs * nz;
      float ox = p.x + ImGui::GetScrollX(), oy = p.y + ImGui::GetScrollY();
      ImGui::SetScrollX(ox - mo.x + relx * nd);            // keep the byte under the cursor fixed
      ImGui::SetScrollY(oy - mo.y + rely * nd);
      g_heatZoom = nz; dim = nd;
    }
    ImGui::Image((ImTextureID)(intptr_t)g_heatTex, ImVec2(dim, dim));
    if (hov && relx >= 0 && relx < 1 && rely >= 0 && rely < 1) {
      int a = ((int)(rely * 256)) * 256 + (int)(relx * 256);
      if (heat) ImGui::SetTooltip("$%04X = $%02X   R:%u W:%u X:%u", a, dbgPeek(a), R[a], W[a], X[a]);
      else      ImGui::SetTooltip("$%04X = $%02X", a, dbgPeek(a));
    }
    ImGui::EndChild();
  }
  ImGui::End();
}

// --- emulator display window ----------------------------------------------------------------------
static void buildDisplayWindow(SDL_Texture *emuTex, int fbW, int fbH)
{
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  if (ImGui::Begin("Emulator")) {
    // source sub-rect of the framebuffer: full 320x240, or just the active video content if cropping
    int sx = 0, sy = 0, sw = fbW, sh = fbH;
    if (g_cropToVideo) {
      int l, t, w, h; desktopGetVideoRect(&l, &t, &w, &h);
      if (w > 0 && h > 0) { sx = l; sy = t; sw = w; sh = h; }
    }
    g_srcRect = SDL_Rect{ sx, sy, sw, sh };

    // aspect-fit the source rect into the window's content area, centered (letterbox/pillarbox)
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float ar = (float)sw / (float)sh;
    float dw = avail.x, dh = dw / ar;
    if (dh > avail.y) { dh = avail.y; dw = dh * ar; }
    ImVec2 cur = ImGui::GetCursorScreenPos();
    ImVec2 pos = ImVec2(cur.x + (avail.x - dw) * 0.5f, cur.y + (avail.y - dh) * 0.5f);
    ImGui::SetCursorScreenPos(pos);

    SDL_SetTextureScaleMode(emuTex, g_filterLinear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    ImVec2 uv0(sx / (float)fbW, sy / (float)fbH);
    ImVec2 uv1((sx + sw) / (float)fbW, (sy + sh) / (float)fbH);
    ImGui::Image((ImTextureID)(intptr_t)emuTex, ImVec2(dw, dh), uv0, uv1);

    g_dstRect = SDL_Rect{ (int)pos.x, (int)pos.y, (int)dw, (int)dh };
  } else {
    g_dstRect = SDL_Rect{ 0, 0, 0, 0 };   // window collapsed -> no touch target
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

// Headless self-check: dump the FULL composited window (ImGui chrome + emulator image), not just the
// 320x240 framebuffer, to a BMP at frame EMU_UI_DUMP_AT. EMU_UI_QUIT=1 exits right after, so the UI
// can be validated offline. Call after the ImGui draw data is rendered, before Present.
static void uiMaybeCapture()
{
  static const char *path = getenv("EMU_UI_DUMP");
  if (!path) return;
  static long frame = -1; frame++;
  static long at = []{ const char *s = getenv("EMU_UI_DUMP_AT"); return s ? atol(s) : 120L; }();
  if (frame < at) return;
  int w = 0, h = 0; SDL_GetRendererOutputSize(g_ren, &w, &h);
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
  if (s) {
    SDL_RenderReadPixels(g_ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
    fprintf(stderr, "[ui] saved %dx%d window capture -> %s\n", w, h, path);
  }
  if (getenv("EMU_UI_QUIT")) {
    if (getenv("EMU_UI_SAVE")) desktopUiSaveState();   // exercise the clean-quit persistence path
    SDL_Quit(); std::exit(0);
  }
}

// --- native SD file browser (System > Load disk / cartridge) ------------------------------------
static std::string lc(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }

static void buildLoadBrowser()
{
  if (!g_showLoad) return;
  ImGui::SetNextWindowSize(ImVec2(560, 440), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Load disk / cartridge", &g_showLoad)) {
    ImGui::TextWrapped("SD: %s", desktopSdRoot());
    ImGui::Text("Platform: %s", dbgPlatformName(dbgPlatform()));
    ImGui::SameLine(); ImGui::TextDisabled("(%s)", dbgFileExts());
    static char filter[64] = "";
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filt", "filter by name...", filter, sizeof(filter));
    ImGui::Separator();

    // current platform's accepted extensions
    std::vector<std::string> exts; { std::string e;
      for (const char *s = dbgFileExts(); ; s++) {
        if (*s == ' ' || *s == 0) { if (!e.empty()) exts.push_back(e); e.clear(); if (*s == 0) break; }
        else e += *s;
      } }
    std::string flc = lc(filter);

    if (ImGui::BeginChild("files")) {
      namespace fs = std::filesystem;
      std::error_code ec;
      std::vector<std::string> names;
      for (fs::directory_iterator it(desktopSdRoot(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string name = it->path().filename().string();
        std::string ext  = lc(it->path().extension().string());      // ".dsk"
        if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
        bool ok = false; for (auto &x : exts) if (x == ext) { ok = true; break; }
        if (!ok) continue;
        if (!flc.empty() && lc(name).find(flc) == std::string::npos) continue;
        names.push_back(name);
      }
      std::sort(names.begin(), names.end());
      for (auto &name : names)
        if (ImGui::Selectable(name.c_str())) {
          std::string sd = "/" + name;
          if (dbgLoadFile(sd.c_str())) g_showLoad = false;   // Apple II / IIGS re-exec here
        }
      if (names.empty()) ImGui::TextDisabled("(no matching files in the SD root)");
    }
    ImGui::EndChild();
  }
  ImGui::End();
}

// --- native settings (System > Settings) --------------------------------------------------------
static void buildSettings()
{
  if (!g_showSettings) return;
  ImGui::SetNextWindowSize(ImVec2(340, 240), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Settings", &g_showSettings)) {
    ImGui::Checkbox("Sound", &sound);
    if (dbgAppleModelSupported()) {
      ImGui::SeparatorText("Apple II");
      bool iie = dbgGetAppleIIe();
      if (ImGui::Checkbox("Enhanced IIe (vs II+)", &iie)) dbgSetAppleIIe(iie);   // reboots the machine
      ImGui::Checkbox("Color video", &videoColor);
      ImGui::Checkbox("Smooth upscale", &smoothUpscale);
    }
    ImGui::Separator();
    if (ImGui::Button("Save to EEPROM")) saveConfig();
    ImGui::SameLine(); ImGui::TextDisabled("changes apply live");
  }
  ImGui::End();
}

// --- disk-read heat map: a track x position-in-track grid (Apple II Disk II) --------------------
// read -> green, write -> red (both -> yellow), same convention as the memory map. log-scaled.
static inline ImU32 diskCellColor(uint32_t rd, uint32_t wr, float lmR, float lmW) {
  if (!rd && !wr) return IM_COL32(24, 24, 32, 255);
  int g = rd ? (int)(255.0f * logf(1.0f + (float)rd) / lmR) : 0;
  int r = wr ? (int)(255.0f * logf(1.0f + (float)wr) / lmW) : 0;
  return IM_COL32(r, g, 36, 255);
}

// HD / ROM case: the original track x position-in-track grid.
static void drawDiskHeatGrid(int ROWS, int COLS, float lmR, float lmW)
{
  ImVec2 avail = ImGui::GetContentRegionAvail();
  const float labelW = 30.0f, ch = 12.0f;
  float cw = (avail.x - labelW) / COLS; if (cw < 3) cw = 3;
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  for (int r = 0; r < ROWS; r++) {
    if (r % 5 == 0) { char t[8]; snprintf(t, sizeof(t), "%d", r);
      dl->AddText(ImVec2(p0.x, p0.y + r * ch), IM_COL32(150, 150, 150, 255), t); }
    for (int c = 0; c < COLS; c++) {
      int i = r * COLS + c;
      ImVec2 a(p0.x + labelW + c * cw, p0.y + r * ch);
      dl->AddRectFilled(a, ImVec2(a.x + cw - 1, a.y + ch - 1), diskCellColor(g_dbgDiskHeat[i], g_dbgDiskHeatW[i], lmR, lmW));
    }
  }
  ImGui::Dummy(ImVec2(avail.x, ROWS * ch + 4));
  if (ImGui::IsItemHovered()) {
    ImVec2 m = ImGui::GetIO().MousePos;
    int c = (int)((m.x - (p0.x + labelW)) / cw), r = (int)((m.y - p0.y) / ch);
    if (r >= 0 && r < ROWS && c >= 0 && c < COLS) { int i = r * COLS + c;
      ImGui::SetTooltip("track %d   pos %d/%d   R:%u  W:%u", r, c, COLS, g_dbgDiskHeat[i], g_dbgDiskHeatW[i]); }
  }
}

// FLOPPY case: a circular platter — concentric rings = tracks (0 = outer edge), angular wedges =
// position-in-track (~sectors), filled by read intensity. The current head track gets a yellow ring.
static void drawDiskHeatCircular(int ROWS, int COLS, float lmR, float lmW)
{
  ImVec2 avail = ImGui::GetContentRegionAvail();
  float sz = (avail.x < avail.y ? avail.x : avail.y); if (sz < 140) sz = 140;
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImVec2 ctr(p0.x + avail.x * 0.5f, p0.y + sz * 0.5f);
  float rMax = sz * 0.5f - 6.0f, rHub = rMax * 0.16f, thick = (rMax - rHub) / ROWS;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float TWO_PI = 6.2831853f, A0 = -1.5708f;   // start angle at 12 o'clock
  dl->AddCircleFilled(ctr, rMax + 2.0f, IM_COL32(8, 8, 12, 255), 64);
  const int SUB = 3;                                  // arc sub-steps per wedge for smooth rings
  for (int t = 0; t < ROWS; t++) {
    float rO = rMax - t * thick, rI = rO - thick + 0.6f;
    for (int b = 0; b < COLS; b++) {
      int i = t * COLS + b;
      ImU32 col = diskCellColor(g_dbgDiskHeat[i], g_dbgDiskHeatW[i], lmR, lmW);
      float aw = TWO_PI / COLS;
      for (int s = 0; s < SUB; s++) {
        float aa = A0 + aw * (b + (float)s / SUB), ab = A0 + aw * (b + (float)(s + 1) / SUB);
        ImVec2 i0(ctr.x + rI * cosf(aa), ctr.y + rI * sinf(aa));
        ImVec2 o0(ctr.x + rO * cosf(aa), ctr.y + rO * sinf(aa));
        ImVec2 o1(ctr.x + rO * cosf(ab), ctr.y + rO * sinf(ab));
        ImVec2 i1(ctr.x + rI * cosf(ab), ctr.y + rI * sinf(ab));
        dl->AddQuadFilled(i0, o0, o1, i1, col);
      }
    }
  }
  if (g_dbgDiskTrack >= 0 && g_dbgDiskTrack < ROWS)   // live head position
    dl->AddCircle(ctr, rMax - g_dbgDiskTrack * thick - thick * 0.5f, IM_COL32(255, 225, 110, 255), 64, 1.6f);
  dl->AddCircleFilled(ctr, rHub, IM_COL32(40, 40, 48, 255), 32);
  dl->AddCircle(ctr, rHub, IM_COL32(90, 90, 100, 255), 32, 1.0f);

  ImGui::Dummy(ImVec2(avail.x, sz));
  if (ImGui::IsItemHovered()) {
    ImVec2 m = ImGui::GetIO().MousePos;
    float dx = m.x - ctr.x, dy = m.y - ctr.y, r = sqrtf(dx * dx + dy * dy);
    if (r <= rMax && r >= rHub) {
      int t = (int)((rMax - r) / thick); if (t < 0) t = 0; if (t >= ROWS) t = ROWS - 1;
      float ang = atan2f(dy, dx) - A0; if (ang < 0) ang += TWO_PI;
      int b = (int)(ang / TWO_PI * COLS) % COLS; int i = t * COLS + b;
      ImGui::SetTooltip("track %d   pos %d/%d   R:%u  W:%u", t, b, COLS, g_dbgDiskHeat[i], g_dbgDiskHeatW[i]);
    }
  }
}

static void buildDiskHeatPanel()
{
  if (!g_showDisk) return;
  if (ImGui::Begin("Disk read", &g_showDisk)) {
    if (!dbgDiskHeatSupported()) {
      ImGui::TextDisabled("(disk-read heat map only for Apple II)");
      ImGui::End(); return;
    }
    bool on = dbgDiskHeatEnabled();
    if (ImGui::Checkbox("Record", &on)) dbgDiskHeatEnable(on);
    ImGui::SameLine(); if (ImGui::Button("Clear")) dbgDiskHeatClear();
    ImGui::SameLine(); ImGui::Checkbox("Fade", &g_diskFade);
    ImGui::SameLine(); ImGui::Text("track: %d", g_dbgDiskTrack);
    if (on && g_diskFade) dbgDiskHeatDecay(0.94f);
    bool floppy = dbgDiskIsFloppy();
    ImGui::TextDisabled(floppy ? "rings = tracks (0 = edge), wedges = sector;  green = read, red = write"
                               : "rows = tracks, cols = position;  green = read, red = write");
    if (!on) { ImGui::TextDisabled("Enable Record, then boot/load from disk."); ImGui::End(); return; }

    const int ROWS = 35, COLS = DBG_DISK_BINS;
    uint32_t mxR = 1, mxW = 1;
    for (int i = 0; i < ROWS * COLS; i++) { if (g_dbgDiskHeat[i] > mxR) mxR = g_dbgDiskHeat[i];
                                            if (g_dbgDiskHeatW[i] > mxW) mxW = g_dbgDiskHeatW[i]; }
    float lmR = logf(1.0f + (float)mxR), lmW = logf(1.0f + (float)mxW);

    if (floppy) drawDiskHeatCircular(ROWS, COLS, lmR, lmW);   // disk image -> circular platter
    else        drawDiskHeatGrid(ROWS, COLS, lmR, lmW);        // HD / block image -> grid
  }
  ImGui::End();
}

void desktopUiFrame(SDL_Texture *emuTex, int fbW, int fbH)
{
  ImGui_ImplSDLRenderer2_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();

  ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGuiID dockId = ImGui::DockSpaceOverViewport(0, vp, ImGuiDockNodeFlags_None);
  if (g_buildLayout) {
    g_buildLayout = false;
    // default layout: emulator fills the left ~2/3, debug panels stack on the right
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, vp->Size);
    ImGuiID rightId, leftId;
    ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Right, 0.34f, &rightId, &leftId);
    ImGui::DockBuilderDockWindow("Emulator", leftId);
    // right column stacked top->bottom: Control, CPU, Memory (all visible at once)
    ImGuiID ctrlId, rest1;
    ImGui::DockBuilderSplitNode(rightId, ImGuiDir_Up, 0.34f, &ctrlId, &rest1);
    ImGui::DockBuilderDockWindow("Control", ctrlId);
    ImGuiID cpuId, memId;
    ImGui::DockBuilderSplitNode(rest1, ImGuiDir_Up, 0.42f, &cpuId, &memId);
    ImGui::DockBuilderDockWindow("CPU", cpuId);
    ImGui::DockBuilderDockWindow("I/O", cpuId);         // tabbed with CPU
    ImGui::DockBuilderDockWindow("Disassembly", memId);
    ImGui::DockBuilderDockWindow("Breakpoints", memId); // tabbed with Disassembly
    ImGui::DockBuilderDockWindow("Memory", memId);
    ImGui::DockBuilderDockWindow("Heat map", memId);
    ImGui::DockBuilderDockWindow("Disk read", memId);
    ImGui::DockBuilderDockWindow("Audio spectrum", memId);
    ImGui::DockBuilderFinish(dockId);
  }

  // debug hotkeys (F5 pause/resume, F10 step) — handled here so they work over the whole window
  if (!ImGui::GetIO().WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false))  dbgSetPaused(!dbgIsPaused());
    if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) dbgStep();
  }

  // Offline self-check: EMU_DBG_STEP=N issues one single-step per frame (so the CPU thread actually
  // runs each), proving stepping advances PC. Paired with EMU_DBG_PAUSE + a capture.
  static int autoStep = []{ const char *s = getenv("EMU_DBG_STEP"); return s ? atoi(s) : 0; }();
  if (autoStep > 0) { dbgStep(); autoStep--; }

  // Offline self-test: EMU_DBG_SWAP=/img.dsk hot-swaps the disk at frame EMU_DBG_SWAP_AT (default 250)
  // to prove it does NOT re-exec (no second "DiskII Setup"/"Ready." in the log).
  static const char *swapTo = getenv("EMU_DBG_SWAP");
  static long swapAt = []{ const char *s = getenv("EMU_DBG_SWAP_AT"); return s ? atol(s) : 250L; }();
  static long swFrame = -1; swFrame++;
  if (swapTo && swFrame == swapAt) { fprintf(stderr, "[swap] -> %s\n", swapTo); dbgLoadFile(swapTo); }
  static long rebootAt = []{ const char *s = getenv("EMU_DBG_REBOOT_AT"); return s ? atol(s) : 0L; }();
  if (rebootAt > 0 && swFrame == rebootAt) { fprintf(stderr, "[reboot] emulator-only reset\n"); dbgReset(); }

  buildMenuBar();
  buildDisplayWindow(emuTex, fbW, fbH);
  buildControlPanel();
  buildCpuPanel();
  buildIoPanel();
  buildDisasmPanel();
  buildBpPanel();
  buildMemoryPanel();
  buildHeatPanel();
  buildDiskHeatPanel();
  buildSpectrumPanel();
  buildLoadBrowser();
  buildSettings();
  desktopUiAutoSaveCfg();   // continuously persist view options (debounced), not just on quit
  if (g_showDemo) ImGui::ShowDemoWindow(&g_showDemo);

  ImGui::Render();
  SDL_SetRenderDrawColor(g_ren, 12, 12, 14, 255);
  SDL_RenderClear(g_ren);
  ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_ren);
  uiMaybeCapture();                    // optional offline screenshot of the full composited window
  SDL_RenderPresent(g_ren);
}

#endif // BOARD_DESKTOP
