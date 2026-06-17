// optionsui.ino - Modern, touch-driven settings window.
//
// Replaces the old Apple II text-grid options screen with a styled, clickable UI
// drawn with TFT_eSPI primitives. Like the on-screen keyboard, all touch reads and
// drawing happen on core 0 from renderLoop() (video.ino) while OptionsWindow is set
// (the CPU is paused). Touch is read through the shared touchRead() in touchkeyboard.ino.
//
// It drives the same emulator state and helpers the PS/2 menu used (HdDisk, AppleIIe,
// Fast1MhzSpeed, sound, joystick, videoColor, upscale, smoothUpscale, volume, the
// disk/HD file lists, setDiskFile/setHdFile, saveEEPROM, ESP.restart), so PS/2 and
// touch stay interchangeable. PS/2 changes call optionsUiMarkDirty() to refresh it.

// ---- Layout (320 x 240) ----
#define OUI_TITLE_H   26
#define OUI_TG_TOP    28          // toggle grid: 4 columns x 2 rows
#define OUI_TG_W      80
#define OUI_TG_H      34
#define OUI_VOL_TOP   98
#define OUI_VOL_H     20
#define OUI_FB_TOP    120         // file browser header
#define OUI_FB_HDR_H  14
#define OUI_FB_LIST   134         // file rows
#define OUI_FB_ROWH   14
#define OUI_FB_ROWS   5
#define OUI_ACT_TOP   208         // action buttons
#define OUI_ACT_H     30

// ---- Palette (macros: evaluated at runtime so tft is already constructed) ----
#define OUI_BG      tft.color565(18, 20, 26)
#define OUI_TITLE   tft.color565(0, 150, 200)
#define OUI_CARD    tft.color565(44, 48, 60)
#define OUI_CARD2   tft.color565(30, 33, 42)
#define OUI_SEL     tft.color565(0, 120, 215)
#define OUI_ON      tft.color565(40, 175, 95)
#define OUI_OFF     tft.color565(150, 160, 175)
#define OUI_TXT     TFT_WHITE
#define OUI_LBL     tft.color565(150, 160, 175)
#define OUI_MOUNT   tft.color565(40, 150, 80)
#define OUI_REBOOT  tft.color565(210, 120, 30)
#define OUI_RED     tft.color565(205, 70, 60)
#define OUI_BORDER  tft.color565(70, 78, 92)

static bool optionsUiDirty       = false;
static bool optionsUiFirstDraw   = false;
static bool optionsUiPrevDown    = false;
static bool optionsUiWaitRelease = false;

// Joystick focus: left/right moves between controls; the focused one gets a white
// border. Order: 0..5 toggle cards, then volume, file list, MOUNT, SAVE & REBOOT.
// Toggle grid slots 6 and 7 (bottom-right) are intentionally left empty for two
// future buttons; navigation skips them.
#define OUI_TG_COUNT   6
#define OUI_FOC_VOL    6
#define OUI_FOC_FILES  7
#define OUI_FOC_MOUNT  8
#define OUI_FOC_REBOOT 9
#define OUI_FOC_COUNT  10
static int optionsUiFocus = 0;

// ---------------------------------------------------------------------------
// Selection / scrolling helpers
// ---------------------------------------------------------------------------
void optionsUiSyncSelection()
{
  std::vector<std::string> &files = HdDisk ? hdFiles : diskFiles;
  std::string sel = (HdDisk ? selectedHdFileName : selectedDiskFileName).c_str();
  int idx = -1;
  for (int i = 0; i < (int)files.size(); i++)
    if (files[i] == sel) { idx = i; break; }
  if (idx < 0) idx = 0;
  shownFile = (uint8_t)idx;
  if (files.empty()) { firstShowFile = 0; return; }
  if (shownFile < firstShowFile) firstShowFile = shownFile;
  else if (shownFile >= firstShowFile + OUI_FB_ROWS) firstShowFile = shownFile - OUI_FB_ROWS + 1;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void ouiSmallBtn(int x, int y, int w, int h, const char *s, uint16_t face)
{
  tft.fillRoundRect(x, y, w, h, 4, face);
  tft.drawRoundRect(x, y, w, h, 4, OUI_BORDER);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(OUI_TXT, face);
  tft.drawString(s, x + w / 2, y + h / 2, 2);
}

// 2px white outline marking the control the joystick is focused on.
static void ouiFocusRing(int x, int y, int w, int h, int r)
{
  tft.drawRoundRect(x,     y,     w,     h,     r, TFT_WHITE);
  tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, r, TFT_WHITE);
}

static void ouiDrawToggle(int idx, const char *label, const char *value, uint16_t valColor)
{
  int col = idx % 4, row = idx / 4;
  int x = col * OUI_TG_W, y = OUI_TG_TOP + row * OUI_TG_H;
  tft.fillRoundRect(x + 2, y + 2, OUI_TG_W - 4, OUI_TG_H - 4, 5, OUI_CARD);
  tft.drawRoundRect(x + 2, y + 2, OUI_TG_W - 4, OUI_TG_H - 4, 5, OUI_BORDER);
  // Keep the ring inside the card's filled area (x+2..) so a repaint erases it when
  // focus moves; drawing it 1px outside would leave white pixels in the inter-card gap.
  if (optionsUiFocus == idx) ouiFocusRing(x + 2, y + 2, OUI_TG_W - 4, OUI_TG_H - 4, 5);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(OUI_LBL, OUI_CARD);
  tft.drawString(label, x + 8, y + 5, 1);
  tft.setTextDatum(BL_DATUM);
  tft.setTextColor(valColor, OUI_CARD);
  tft.drawString(value, x + 8, y + OUI_TG_H - 5, 2);
}

static void ouiDrawToggles()
{
  ouiDrawToggle(0, "DEVICE",   HdDisk ? "HD" : "DISK",          OUI_TXT);
  ouiDrawToggle(1, "MACHINE",  AppleIIe ? "IIe" : "II+",        OUI_TXT);
  ouiDrawToggle(2, "SPEED",    Fast1MhzSpeed ? "FAST" : "1MHz", OUI_TXT);
  ouiDrawToggle(3, "SOUND",    sound ? "ON" : "MUTE",           OUI_TXT);
  ouiDrawToggle(4, "JOYSTICK", joystick ? "ON" : "OFF",         OUI_TXT);
  ouiDrawToggle(5, "VIDEO",    videoColor ? "COLOR" : "MONO",   OUI_TXT);
  // Slots 6 and 7 (bottom-right) left empty for two future buttons.
}

static void ouiDrawVolume()
{
  tft.fillRect(0, OUI_VOL_TOP, 320, OUI_VOL_H, OUI_BG);
  int level = volume / 0x10;
  char vlabel[24];
  sprintf(vlabel, "VOLUME  %d/15", level);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(OUI_LBL, OUI_BG);
  tft.drawString(vlabel, 7, OUI_VOL_TOP + OUI_VOL_H / 2, 1);

  int by = OUI_VOL_TOP + 1, bh = OUI_VOL_H - 2;
  ouiSmallBtn(120, by, 28, bh, "-", OUI_CARD);
  ouiSmallBtn(288, by, 28, bh, "+", OUI_CARD);

  int tx = 152, tw = 130, th = 8, ty = by + (bh - th) / 2;
  tft.fillRoundRect(tx, ty, tw, th, 3, OUI_CARD2);
  int fw = (int)((long)tw * level / 15);
  if (fw > 0) tft.fillRoundRect(tx, ty, fw, th, 3, OUI_SEL);

  if (optionsUiFocus == OUI_FOC_VOL) ouiFocusRing(116, OUI_VOL_TOP, 202, OUI_VOL_H, 4);
}

static void ouiDrawFiles()
{
  std::vector<std::string> &files = HdDisk ? hdFiles : diskFiles;
  std::string sel = (HdDisk ? selectedHdFileName : selectedDiskFileName).c_str();

  // header
  tft.fillRect(0, OUI_FB_TOP, 320, OUI_FB_HDR_H, OUI_BG);
  char hdr[40];
  sprintf(hdr, "%s IMAGES  (%d)", HdDisk ? "HD" : "DISK", (int)files.size());
  tft.setTextDatum(BL_DATUM);
  tft.setTextColor(OUI_LBL, OUI_BG);
  tft.drawString(hdr, 7, OUI_FB_TOP + OUI_FB_HDR_H - 2, 1);

  int listH = OUI_FB_ROWS * OUI_FB_ROWH;
  if (files.empty()) {
    tft.fillRect(0, OUI_FB_LIST, 300, listH, OUI_CARD2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(OUI_LBL, OUI_CARD2);
    tft.drawString("No images on SD card", 150, OUI_FB_LIST + listH / 2, 1);
  } else {
    for (int r = 0; r < OUI_FB_ROWS; r++) {
      int idx = firstShowFile + r;
      int ry = OUI_FB_LIST + r * OUI_FB_ROWH;
      if (idx >= (int)files.size()) { tft.fillRect(0, ry, 300, OUI_FB_ROWH, OUI_CARD2); continue; }

      bool selected = (idx == shownFile);
      bool mounted  = (files[idx] == sel);
      uint16_t rowbg = selected ? OUI_SEL : OUI_CARD2;
      tft.fillRect(0, ry, 300, OUI_FB_ROWH, rowbg);
      if (mounted) tft.fillRect(0, ry, 3, OUI_FB_ROWH, OUI_ON);

      std::string nm = files[idx];
      if (!nm.empty() && nm[0] == '/') nm = nm.substr(1);
      if (nm.size() > 46) nm = nm.substr(0, 43) + "...";
      tft.setTextDatum(ML_DATUM);
      tft.setTextColor(selected ? OUI_TXT : tft.color565(200, 205, 215), rowbg);
      tft.drawString(nm.c_str(), 9, ry + OUI_FB_ROWH / 2, 1);
    }
  }

  // scroll buttons (right column)
  int sX = 302, sW = 18, half = listH / 2;
  ouiSmallBtn(sX, OUI_FB_LIST, sW, half - 1, "^", OUI_CARD);
  ouiSmallBtn(sX, OUI_FB_LIST + half + 1, sW, half - 1, "v", OUI_CARD);

  if (optionsUiFocus == OUI_FOC_FILES) ouiFocusRing(0, OUI_FB_LIST, 300, listH, 2);
}

static void ouiDrawActions()
{
  std::vector<std::string> &files = HdDisk ? hdFiles : diskFiles;
  bool canMount = !files.empty();
  int y = OUI_ACT_TOP, h = OUI_ACT_H;

  uint16_t mc = canMount ? OUI_MOUNT : OUI_CARD2;
  tft.fillRoundRect(6, y, 120, h, 6, mc);
  tft.drawRoundRect(6, y, 120, h, 6, OUI_BORDER);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(canMount ? OUI_TXT : OUI_LBL, mc);
  tft.drawString("MOUNT", 66, y + h / 2, 2);

  tft.fillRoundRect(132, y, 182, h, 6, OUI_REBOOT);
  tft.drawRoundRect(132, y, 182, h, 6, OUI_BORDER);
  tft.setTextColor(OUI_TXT, OUI_REBOOT);
  tft.drawString("SAVE & REBOOT", 223, y + h / 2, 2);

  if (optionsUiFocus == OUI_FOC_MOUNT)  ouiFocusRing(6, y, 120, h, 6);
  if (optionsUiFocus == OUI_FOC_REBOOT) ouiFocusRing(132, y, 182, h, 6);
}

static void ouiDrawTitle()
{
  tft.fillRect(0, 0, 320, OUI_TITLE_H, OUI_TITLE);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(OUI_TXT, OUI_TITLE);
  tft.drawString("APPLE II  SETTINGS", 10, OUI_TITLE_H / 2, 2);
  int cw = OUI_TITLE_H, cx = 320 - cw;
  tft.fillRect(cx, 0, cw, OUI_TITLE_H, OUI_RED);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(OUI_TXT, OUI_RED);
  tft.drawString("X", cx + cw / 2, OUI_TITLE_H / 2, 2);
}

void optionsUiRender()
{
  if (!optionsUiDirty) return;
  if (optionsUiFirstDraw) { tft.fillScreen(OUI_BG); optionsUiFirstDraw = false; }
  ouiDrawTitle();
  ouiDrawToggles();
  ouiDrawVolume();
  ouiDrawFiles();
  ouiDrawActions();
  optionsUiDirty = false;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
static void ouiToggle(int idx)
{
  switch (idx) {
    case 0:
      HdDisk = !HdDisk;
      // Only the boot device's image list is loaded at startup; scan the other on
      // demand (synchronously) so HD/DISK mode always shows its files.
      if (HdDisk) { if (hdFiles.empty())   loadHdFilesSync();   }
      else        { if (diskFiles.empty()) loadDiskFilesSync(); }
      shownFile = 0xff; firstShowFile = 0; optionsUiSyncSelection();
      break;
    case 1: AppleIIe = !AppleIIe; activeFlags = AppleIIe ? flagsIIe : flagsIIplus; break;
    case 2: Fast1MhzSpeed = !Fast1MhzSpeed; break;
    case 3: sound = !sound; break;
    case 4: joystick = !joystick; break;
    case 5: videoColor = !videoColor; break;
    default: return;
  }
  optionsUiDirty = true;
}

static void ouiScroll(int dir)
{
  std::vector<std::string> &files = HdDisk ? hdFiles : diskFiles;
  int maxStart = (int)files.size() - OUI_FB_ROWS;
  if (maxStart < 0) maxStart = 0;
  int fs = (int)firstShowFile + dir;
  if (fs < 0) fs = 0;
  if (fs > maxStart) fs = maxStart;
  firstShowFile = (uint8_t)fs;
  optionsUiDirty = true;
}

static void ouiMount()
{
  std::vector<std::string> &files = HdDisk ? hdFiles : diskFiles;
  if (files.empty()) return;
  if (HdDisk) setHdFile(); else setDiskFile();
  diskChanged = true;
  showHideOptionsWindow();   // mount selected image and close
}

static void ouiSaveReboot()
{
  // Apply the highlighted image to the selected device, persist everything (toggles,
  // volume, both filenames), then reboot so the chosen image is mounted on next boot.
  if (HdDisk) setHdFile(); else setDiskFile();
  saveConfig();
  ESP.restart();
}

// ---- Joystick navigation (called from joystick.ino, core 0) ----
// Left/right move the focus; up/down act on the focused control; fire activates it.
void optionsUiNav(int dir)            // dir: -1 = left, +1 = right
{
  optionsUiFocus = (optionsUiFocus + dir + OUI_FOC_COUNT) % OUI_FOC_COUNT;
  optionsUiDirty = true;
}

void optionsUiAdjust(int dir)         // dir: -1 = up, +1 = down
{
  int f = optionsUiFocus;
  if (f >= 0 && f < OUI_TG_COUNT) { ouiToggle(f); return; }
  if (f == OUI_FOC_VOL) {
    if (dir < 0) { if (volume < 0xf0) volume += 0x10; }
    else         { if (volume > 0) { volume -= 0x10; if (volume > 0xf0) volume = 0; } }
    optionsUiDirty = true;
    return;
  }
  if (f == OUI_FOC_FILES) {
    std::vector<std::string> &files = HdDisk ? hdFiles : diskFiles;
    if (files.empty()) return;
    int idx = (int)shownFile + (dir < 0 ? -1 : 1);
    if (idx < 0) idx = 0;
    if (idx > (int)files.size() - 1) idx = (int)files.size() - 1;
    shownFile = (uint8_t)idx;
    if (shownFile < firstShowFile) firstShowFile = shownFile;
    else if (shownFile >= firstShowFile + OUI_FB_ROWS) firstShowFile = shownFile - OUI_FB_ROWS + 1;
    optionsUiDirty = true;
  }
  // MOUNT / REBOOT have nothing to adjust
}

void optionsUiActivate()              // joystick fire button on the focused control
{
  int f = optionsUiFocus;
  if (f >= 0 && f < OUI_TG_COUNT) ouiToggle(f);
  else if (f == OUI_FOC_FILES)  ouiMount();
  else if (f == OUI_FOC_MOUNT)  ouiMount();
  else if (f == OUI_FOC_REBOOT) ouiSaveReboot();
  // FOC_VOL: nothing (adjust with up/down)
}

static void ouiHandleTap(int16_t x, int16_t y)
{
  // close button
  if (y < OUI_TITLE_H && x >= 320 - OUI_TITLE_H) { showHideOptionsWindow(); return; }

  // toggle grid
  if (y >= OUI_TG_TOP && y < OUI_TG_TOP + 2 * OUI_TG_H) {
    int col = x / OUI_TG_W, row = (y - OUI_TG_TOP) / OUI_TG_H;
    int idx = row * 4 + col;
    if (idx < OUI_TG_COUNT) ouiToggle(idx);   // slots 6,7 are empty
    return;
  }

  // volume +/-
  if (y >= OUI_VOL_TOP && y < OUI_VOL_TOP + OUI_VOL_H) {
    if (x >= 120 && x < 148) { if (volume > 0) { volume -= 0x10; if (volume > 0xf0) volume = 0; } optionsUiDirty = true; return; }
    if (x >= 288 && x < 316) { if (volume < 0xf0) volume += 0x10; optionsUiDirty = true; return; }
  }

  // file list / scroll
  int listH = OUI_FB_ROWS * OUI_FB_ROWH;
  if (y >= OUI_FB_LIST && y < OUI_FB_LIST + listH) {
    if (x >= 302) { ouiScroll(y < OUI_FB_LIST + listH / 2 ? -1 : 1); return; }
    if (x < 300) {
      std::vector<std::string> &files = HdDisk ? hdFiles : diskFiles;
      int idx = firstShowFile + (y - OUI_FB_LIST) / OUI_FB_ROWH;
      if (idx < (int)files.size()) { shownFile = (uint8_t)idx; optionsUiDirty = true; }
      return;
    }
  }

  // action buttons
  if (y >= OUI_ACT_TOP && y < OUI_ACT_TOP + OUI_ACT_H) {
    if (x >= 6 && x < 126) ouiMount();
    else if (x >= 132 && x < 314) ouiSaveReboot();
  }
}

// ---------------------------------------------------------------------------
// Per-frame service (renderLoop, core 0) + entry points for the rest of the app
// ---------------------------------------------------------------------------
void optionsUiPoll()
{
  int16_t x = 0, y = 0;
  bool down = touchRead(&x, &y);
  if (optionsUiWaitRelease) {            // ignore the touch that opened the window
    if (!down) optionsUiWaitRelease = false;
    optionsUiPrevDown = down;
    return;
  }
  if (down && !optionsUiPrevDown) ouiHandleTap(x, y);
  optionsUiPrevDown = down;
}

void optionsUiOpen()
{
  optionsUiSyncSelection();
  optionsUiFocus       = 0;
  optionsUiFirstDraw   = true;
  optionsUiDirty       = true;
  optionsUiWaitRelease = true;   // don't treat the opening tap as a click
  optionsUiPrevDown    = true;
}

// Called by the PS/2 menu handlers (optionsScreenRender/listFiles) so keyboard
// changes refresh the touch UI and keep the selected file visible.
void optionsUiMarkDirty()
{
  if (shownFile != 0xff) {
    if (shownFile < firstShowFile) firstShowFile = shownFile;
    else if (shownFile >= firstShowFile + OUI_FB_ROWS) firstShowFile = shownFile - OUI_FB_ROWS + 1;
  }
  optionsUiDirty = true;
}
