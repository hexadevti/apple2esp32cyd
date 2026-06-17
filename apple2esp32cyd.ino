#include "config.h"

// #include <XPT2046_Touchscreen.h>
// #define TOUCH_CS_PIN  38
// XPT2046_Touchscreen ts(TOUCH_CS_PIN);
// static SPIClass fspi { FSPI };

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Turn off green LED
  logSetup();
  epromSetup();
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

  // MOSI=11, MISO=12, SCK=13
  //ts.begin();
  // ts.begin(fspi); // use alternate SPI port
  // ts.setRotation(1);
  
}

void loop() {
  cpuLoop();
}
