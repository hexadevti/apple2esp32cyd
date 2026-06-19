#pragma once
// Nintendo Entertainment System core — placeholder.
//
// To bring this platform online:
//   1. Implement nesSetup() / nesLoop() (6502, PPU, APU, cartridge mapper, etc.).
//   2. In src/shared/video.cpp splashService(), enable the "NES" button
//      (splashDrawBtn(PLATFORM_NES, "NES", true)) and route its tap to
//      splashSelect(PLATFORM_NES).
//   3. In apple2esp32cyd.ino, branch setup()/loop() on currentPlatform to call
//      nesSetup()/nesLoop().
void nesSetup();
void nesLoop();
