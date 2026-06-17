# apple2esp32cyd

Apple II (II+ / IIe) emulator for the ESP32 **Cheap Yellow Display** (ESP32 + ILI9341 TFT, e.g. the ESP32-2432S028) with SD-card storage.

This is a TFT-only port derived from [hexadevti/Apple2Esp32](https://github.com/hexadevti/Apple2Esp32). The ESP32-S3 (`TFT_S3`), VGA, DAC audio, and LittleFS build paths have been removed, leaving a single **ESP32 + TFT_eSPI + SD** configuration.

## Hardware

- ESP32 (CYD board)
- ILI9341 320x240 TFT — configured through `User_Setup.h` (TFT_eSPI)
- SD card over SPI
- PS/2 keyboard, analog joystick, and speaker

Pin assignments live in [`config.h`](config.h).

## Build

Plain ESP32 target (PSRAM disabled). The Arduino-CLI build/upload commands are in [`.vscode/tasks.json`](.vscode/tasks.json) — by default it compiles for `esp32:esp32:esp32` and uploads on `COM4`. Adjust the FQBN and port to match your board.
