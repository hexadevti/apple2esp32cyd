# apple2esp32cyd

Apple II (II+ / IIe) emulator for the ESP32 **Cheap Yellow Display** (CYD) — a self‑contained
hardware Apple II that boots DOS 3.3 / ProDOS disk images straight off a microSD card and
renders to the on‑board ILI9341 TFT.

This is a **TFT‑only** port derived from [hexadevti/Apple2Esp32](https://github.com/hexadevti/Apple2Esp32).
The ESP32‑S3 (`TFT_S3`), VGA, DAC‑audio and LittleFS build paths from upstream have been removed,
leaving a single **ESP32 + TFT_eSPI + microSD** configuration.

- **Primary target:** [ESP32‑2432S024](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024) (2.4″ CYD, ILI9341, 240×320).
- **Also compatible:** ESP32‑2432S028 (2.8″ CYD) — it shares the same ILI9341 driver and pin map.

---

## Table of contents

- [Features](#features)
- [Hardware](#hardware)
- [Board resources & schematics](#board-resources--schematics)
- [Pin assignments](#pin-assignments)
- [Display configuration (TFT_eSPI)](#display-configuration-tft_espi)
- [Software prerequisites](#software-prerequisites)
- [Build & flash](#build--flash)
- [microSD card preparation](#microsd-card-preparation)
- [Supported image formats](#supported-image-formats)
- [Controls](#controls)
- [On‑screen menu](#on-screen-menu)
- [Power & battery](#power--battery)
- [Project structure](#project-structure)
- [Credits & license](#credits--license)

---

## Features

- **6502 CPU** core with full documented opcode set.
- **Apple II+ and Apple IIe** machine models, switchable at runtime (80‑column / aux‑RAM, language card, IOU softswitches).
- **Disk II** controller — boots 5.25″ 140 KB floppy images (`.dsk` / `.do` / `.po`), with track write‑back to SD.
- **ProDOS hard‑disk** (block device) — mount large `.hdv` / `.po` / `.2mg` images.
- **Video:** LoRes, HiRes, text 40/80‑column, page 1/2, mixed/split — color or monochrome, with optional up‑scaling / smooth up‑scaling for the 240×320 panel.
- **Sound:** Apple speaker emulation through the board's audio amplifier and speaker; volume control; mute.
- **Input:** PS/2 keyboard and an analog joystick / paddles with buttons.
- **Mouse** (AppleMouse‑style) state plumbing.
- **On‑screen options menu** (file browser + machine settings) and a built‑in **6502 debugger** (step / breakpoint / stack trace).
- **Settings persistence** in EEPROM (machine type, speed, sound, joystick, selected disk/HD image, etc.).
- Apple II/IIe **ROMs are embedded** in [`rom.h`](rom.h) — no external ROM files needed.

---

## Hardware

- **MCU:** ESP32‑WROOM‑32 (dual‑core, 4 MB flash, PSRAM **disabled**).
- **Display:** 2.4″ ILI9341 TFT, 240×320, SPI (HSPI bus), software‑controlled backlight.
- **Touch:** XPT2046 (resistive `…S024R`) or CST820 (capacitive `…S024C`) — *not required by the emulator* (touch is currently unused; input is PS/2 + joystick).
- **Storage:** microSD over its own SPI bus (VSPI).
- **Audio:** SC8002B amplifier + on‑board speaker (JST 1.25 mm).
- **Power:** USB‑C (CH340C USB‑UART for flashing) + Li‑ion battery charge/boost circuit (JST 1.25 mm).
- **Extras on board:** RGB LED, LDR light sensor, BOOT/RST buttons, 1.25 mm GPIO break‑out headers.

> The emulator's **PS/2 keyboard, analog joystick and speaker** attach to GPIOs that the CYD
> brings out on its 1.25 mm headers (`P1`, `P3`, `CN1`) and on‑board peripherals. See the pin table
> below — some of these pins are shared with on‑board peripherals (e.g. the RGB LED on `IO4`), so
> wire external peripherals accordingly.

---

## Board resources & schematics

All board documentation lives in the manufacturer repo
[**jpduhen/CYD_2.4inch_ESP32-2432S024**](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024):

| Resource | Location in board repo |
| --- | --- |
| Schematic — sheet 1 (power, charging, SD, RGB LED, ADC) | [`5-Schematic/ESP32-2432S024-1-V1.0.png`](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024/blob/main/5-Schematic/ESP32-2432S024-1-V1.0.png) |
| Schematic — sheet 2 (ESP32, display, touch, audio) | [`5-Schematic/2432S024-2-V1.0.png`](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024/blob/main/5-Schematic/2432S024-2-V1.0.png) |
| ESP32‑WROOM‑32 pin definition | [`5-Schematic/ESP32-WROOM-1 Pin definition.png`](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024/blob/main/5-Schematic/ESP32-WROOM-1%20Pin%20definition.png) |
| Board specification (EN) | [`2-Specification/ESP32-2432S024 Specifications-EN.pdf`](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024/blob/main/2-Specification/ESP32-2432S024%20Specifications-EN.pdf) |
| Getting‑started guide | [`6-User_Manual/Getting started 2.4 Inch.pdf`](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024/blob/main/6-User_Manual/Getting%20started%202.4%20Inch.pdf) |
| Component datasheets (ESP32, flash, audio amp, charger, DHT11) | [`4-Driver_IC_Data_Sheet/`](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024/tree/main/4-Driver_IC_Data_Sheet) |
| Factory demos + reference `User_Setup.h` | [`1-Demo/Demo_Arduino/`](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024/tree/main/1-Demo/Demo_Arduino) |
| Flash download tool | [`8-Burn operation/`](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024/tree/main/8-Burn%20operation) |

---

## Pin assignments

### Display — ILI9341 (defined in [`User_Setup.h`](User_Setup.h))

| Signal | GPIO |
| --- | --- |
| MISO (SDO) | 12 |
| MOSI (SDI) | 13 |
| SCLK | 14 |
| CS | 15 |
| DC (RS) | 2 |
| RST | — (tied to ESP reset) |
| Backlight (BL) | 27 (active HIGH) |
| Touch CS (XPT2046) | 33 |
| Touch IRQ (XPT2046) | 36 *(not used by emulator)* |

### Emulator peripherals & storage (defined in [`config.h`](config.h))

| Function | GPIO |
| --- | --- |
| microSD SCK | 18 |
| microSD MISO | 19 |
| microSD MOSI | 23 |
| microSD CS | 5 |
| PS/2 keyboard DATA | 21 |
| PS/2 keyboard CLK/IRQ | 22 |
| Joystick analog X | 4 *(shared with on‑board RGB‑LED red)* |
| Joystick analog Y | 35 (input‑only) |
| Joystick button(s) | 34 (input‑only; on‑board LDR pin) |
| Speaker / audio out | 26 |
| Status LED (`LED_PIN`) | 17 (on‑board RGB‑LED channel) |

### On‑board peripherals (from schematic, for reference)

| Peripheral | GPIO |
| --- | --- |
| RGB LED | 4 (R), 16 (G), 17 (B) — common‑anode, active‑LOW |
| LDR (light sensor, GT36516) | 34 (ADC) |
| Audio amp (SC8002B) input | 26 |
| Capacitive‑touch variant (CST820) | INT 21, RST 25, SCL 32, SDA 33 |
| USB‑UART (CH340C) | TX `U0TXD`, RX `U0RXD` |
| BOOT button | 0 |
| Break‑out headers (`P3`/`CN1`, 1.25 mm) | 21, 22, 35 (+3V3/GND) |
| Serial/power header (`P1`, 1.25 mm) | 5 V, TXD2, RXD2, GND |

---

## Display configuration (TFT_eSPI)

This project ships its own [`User_Setup.h`](User_Setup.h) for the CYD. TFT_eSPI is configured at
**library** level, so before building you must make the library use this file:

1. Locate the installed `TFT_eSPI` library folder
   (e.g. `~/Arduino/libraries/TFT_eSPI` or `Documents/Arduino/libraries/TFT_eSPI`).
2. Replace its `User_Setup.h` with the [`User_Setup.h`](User_Setup.h) from this repo
   (back up the original first).

Key settings in the provided file:

- Driver: `ILI9341_2_DRIVER` (alternative ILI9341 driver; the stock board uses `ILI9341_DRIVER` — either works).
- `TFT_WIDTH 240`, `TFT_HEIGHT 320`.
- `SPI_FREQUENCY 80000000` (80 MHz; drop to 55 MHz / 40 MHz if you see display corruption).
- Backlight on `TFT_BL 27`, `TFT_BACKLIGHT_ON HIGH`.

---

## Software prerequisites

- **arduino‑cli** (or the Arduino IDE).
- **ESP32 Arduino core** `esp32:esp32` — tested with **2.0.17** (`espressif@2.0.17`).
- **Libraries** (see [`install.txt`](install.txt)):
  - `TFT_eSPI` **2.5.43**
  - `ESP32Lib` 0.3.4 *(upstream dependency; not needed for this TFT‑only build, listed for reference)*

Install the core and library:

```bash
arduino-cli core install esp32:esp32@2.0.17
arduino-cli lib install "TFT_eSPI@2.5.43"
# then copy this repo's User_Setup.h into the TFT_eSPI library folder (see above)
```

---

## Build & flash

The Arduino‑CLI build/upload commands live in [`.vscode/tasks.json`](.vscode/tasks.json).
Default target is `esp32:esp32:esp32` on `COM4` — adjust the port/FQBN to match your machine.

**Build:**

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32:PSRAM=disabled,PartitionScheme=default,CPUFreq=240,FlashMode=qio,FlashFreq=80,FlashSize=4M,UploadSpeed=921600,LoopCore=1,EventsCore=1,DebugLevel=none \
  --build-property compiler.optimization_flags=-O2 \
  .
```

**Upload** (replace `COM4` with your serial port):

```bash
arduino-cli upload -p COM4 \
  --fqbn esp32:esp32:esp32:PSRAM=disabled,PartitionScheme=default,CPUFreq=240,FlashMode=qio,FlashFreq=80,FlashSize=4M,UploadSpeed=921600,LoopCore=1,EventsCore=1,DebugLevel=none \
  .
```

In VS Code: **Terminal → Run Task… → “Arduino: Build”** (the default build task, `Ctrl+Shift+B`)
or **“Arduino: Build & Upload”**. Serial monitor runs at **115200** baud.

> **PSRAM is disabled** and the partition scheme is the default 4 MB layout — keep these to match the WROOM‑32 module.

---

## microSD card preparation

1. Format a microSD card as **FAT32**.
2. Copy your disk / hard‑disk images to the card root.
3. Insert the card and power on; pick an image from the on‑screen menu (`Ctrl`+`Esc`).

Sample images are included in [`data/`](data/) (copy them to the SD root), e.g. DOS 3.3,
ProDOS 2.4.2, Lode Runner, Karateka, Ghostbusters. On first boot the default disk is `/karateka.dsk`.

---

## Supported image formats

| Type | Extensions | Notes |
| --- | --- | --- |
| 5.25″ floppy (Disk II) | `.dsk`, `.do`, `.po` | 140 KB; DOS 3.3 order (`.dsk`/`.do`) and ProDOS order (`.po`) |
| Hard disk (ProDOS block device) | `.hdv`, `.po`, `.2mg` | large volumes |

---

## Controls

Input is via a **PS/2 keyboard** and an **analog joystick**. Global shortcuts:

| Keys | Action |
| --- | --- |
| `Ctrl` + `Esc` | Open / close the on‑screen options menu |
| `Ctrl` + `F12` | Apple **Reset** (CPU reset) |
| `Ctrl` + `F5` | Reboot the ESP32 |

---

## On‑screen menu

Open with `Ctrl`+`Esc` (emulation pauses while open). The file list shows the disks/HD images on
the SD card; use it together with the function keys:

| Key | Toggle |
| --- | --- |
| `F1` | **HD** ↔ **DISK** (which device the file list manages) |
| `F2` | **IIe** ↔ **II+** machine model |
| `F3` | **Fast** ↔ **1 MHz** CPU speed |
| `F4` | **Speaker** ↔ **Mute** |
| `F5` | **Joystick** on ↔ off |
| `F6` | **Color** ↔ **Mono** video |
| `F7` | **Upscale** ↔ **Regular** |
| `F8` | **Smooth upscale** ↔ **Regular** |
| `Enter` | Mount the highlighted image |
| `Ctrl` + `Enter` | Save settings/selection to EEPROM and reboot |
| `Esc` | Exit the menu |

A built‑in **6502 debugger** is also available (pause/continue, single‑step `F10`, set breakpoint by
address, and a live instruction/stack trace).

---

## Power & battery

The board is powered over **USB‑C** and can run from a single‑cell **3.7 V Li‑ion/LiPo** battery on
the **JST 1.25 mm** connector. Charging is automatic: plug in USB and the on‑board charge/boost IC
charges the cell and powers the board simultaneously; on‑board LEDs indicate charge status.

> ⚠️ **Single‑cell Li‑ion/LiPo only.** The connector is **JST 1.25 mm** (not the common JST‑PH 2.0 mm).
> Verify **BAT+/BAT‑ polarity against the silkscreen/schematic** before plugging in — reversed leads
> can destroy the board. Exact charge current is in the board spec / charger datasheet (see
> [Board resources](#board-resources--schematics)).

---

## Project structure

| File | Purpose |
| --- | --- |
| [`apple2esp32cyd.ino`](apple2esp32cyd.ino) | `setup()` / `loop()` — boot sequence and main CPU loop |
| [`config.h`](config.h) | Global config: includes, board pins, feature flags, emulator state |
| [`cpu.ino`](cpu.ino) | 6502 CPU core |
| [`memory.ino`](memory.ino) | RAM/ROM map and bank switching |
| [`languagecard.ino`](languagecard.ino) | Language‑card / bank‑switched RAM |
| [`softswitches.ino`](softswitches.ino) | `$C0xx` soft switches |
| [`video.ino`](video.ino) | Text/LoRes/HiRes rendering to the TFT |
| [`disk.ino`](disk.ino) | Disk II controller / floppy images |
| [`hd.ino`](hd.ino) | ProDOS hard‑disk block device |
| [`keyboardPs2.ino`](keyboardPs2.ino) | PS/2 keyboard + global shortcuts |
| [`joystick.ino`](joystick.ino) | Analog joystick / paddles |
| [`mouse.ino`](mouse.ino) | Mouse state |
| [`speaker.ino`](speaker.ino) | Speaker / audio output |
| [`sd.ino`](sd.ino) | microSD / filesystem access |
| [`eprom.ino`](eprom.ino) | EEPROM settings persistence |
| [`interface.ino`](interface.ino) | On‑screen menu, file browser, 6502 debugger |
| [`log.ino`](log.ino) | Serial logging helpers |
| [`rom.h`](rom.h) | Embedded Apple II/IIe ROMs |
| [`User_Setup.h`](User_Setup.h) | TFT_eSPI configuration for this board |
| [`data/`](data/) | Sample disk images for the SD card |

---

## Credits & license

- Upstream emulator: [hexadevti/Apple2Esp32](https://github.com/hexadevti/Apple2Esp32).
- Board documentation: [jpduhen/CYD_2.4inch_ESP32-2432S024](https://github.com/jpduhen/CYD_2.4inch_ESP32-2432S024).

Refer to the upstream project for licensing terms.
