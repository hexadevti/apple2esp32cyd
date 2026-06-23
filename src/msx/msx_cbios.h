// msx_cbios.h - embedded C-BIOS fallback (MSX1 main BIOS) for emu6502.
//
// C-BIOS is a free, BSD-licensed, redistributable MSX BIOS replacement. It boots MSX BASIC but has
// NO Disk BASIC (the disk drive needs a real Disk ROM from SD - see the M5 plan). When the SD card
// has no real MSX BIOS, msxSetup() falls back to this flash-resident array (zero DRAM cost, like
// src/iigs/iigs_rom01.cpp embeds ROM 01).
//
// To populate it: drop cbios_main_msx1.rom (32 KB, from https://cbios.sourceforge.net/) and convert
// it to the C array in msx_cbios.cpp (e.g. `xxd -i cbios_main_msx1.rom`). Until then the length is
// 0 and msxSetup requires an SD-supplied BIOS to boot.

#pragma once
#include <stdint.h>

extern const unsigned int  cbiosMainMsx1Len;   // 0 until the C-BIOS image is embedded
extern const unsigned char cbiosMainMsx1[];     // 32 KB MSX1 main BIOS when present
