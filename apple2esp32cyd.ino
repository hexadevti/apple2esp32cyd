#include "config.h"

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Turn off green LED
  logSetup();
  epromSetup();   // loads currentPlatform (and all saved settings) from EEPROM

  // Display, touch, audio and joystick are shared across platforms; the calls below
  // initialise the Apple II core. When the C64/NES cores are added, branch the
  // core-specific init on currentPlatform here (the splash reboots after a change).
  memoryAlloc();
  FSSetup();
  diskSetup();
  HDSetup();
  videoSetup();
  keyboardSetup();
  oskSetup();

  speakerSetup();
  //wifiSetup();

  joystickSetup();
  printLog("Ready.");
}

void loop() {
  // Platform dispatch: each emulator core has its own main loop. Only the Apple II
  // core exists today; C64 / NES are selected on the boot splash (video.ino) and
  // will plug in here once implemented.
  switch (currentPlatform) {
    case PLATFORM_APPLE2: cpuLoop(); break;
    // case PLATFORM_C64:  c64Loop(); break;   // TODO
    // case PLATFORM_NES:  nesLoop(); break;    // TODO
    default:              cpuLoop(); break;
  }
}
