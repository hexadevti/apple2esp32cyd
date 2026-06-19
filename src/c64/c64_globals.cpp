#include "../../emu.h"
#include "c64.h"

// Definitions for the C64 state shared across the C64 translation units (cpu/memory/vic).
// CPU registers + decode tables stay file-local in c64_cpu.cpp; VIC scratch stays in
// c64_vic.cpp. This file holds the cross-file state (memory, banking, VIC registers).

namespace c64 {

bool mos65c02 = false;   // C64 is NMOS 6510

// memory / banking
unsigned char *ram = nullptr;
bool bankARAM = true, bankDRAM = true, bankERAM = true, bankDIO = true;
uint8_t register1 = 0x37;

// cartridge (.crt)
uint8_t *cartROML = nullptr, *cartROMH = nullptr;
bool cartActive = false, cartExrom = true, cartGame = true;
volatile bool c64ResetReq = false;

// VIC-II state
uint8_t vicreg[0x40];
uint8_t latchd011 = 0, latchd012 = 0;
uint16_t vicmem = 0, bitmapstart = 0x2000, screenmemstart = 1024, rasterline = 0;
uint8_t syncd020 = 0;
bool screenblank = false, badlinecond0 = false;
// VIC framebuffer is 8-bit indexed (C64 colour 0-15) and SPLIT into two halves so it fits
// the fragmented heap (no contiguous 64K block exists alongside the 64K C64 RAM). `bitmap`
// is repointed at the current scanline's start (in the right half) before each
// drawRasterline; all VIC writes are line-relative. c64RenderFrame converts the indices to
// RGB565 (via c64Colors) when pushing to the TFT.
// 8-bit indexed framebuffer, two ~32K halves, MALLOC'd on the C64 path only (in vicSetup,
// before the 64K C64 RAM so the contiguous blocks still fit). NOT static: a static BSS array
// would be reserved for the Apple path too and starve its 128K RAM allocation (boot crash).
uint8_t *bitmap = nullptr;          // -> current scanline (repointed per line); non-null = on
uint8_t *fbTop  = nullptr;          // lines 0..99
uint8_t *fbBot  = nullptr;          // lines 100..199
uint8_t *colormap = nullptr;
const uint8_t *charset = nullptr;
const uint8_t *chrom = nullptr;
uint8_t cntRefreshs = 0;

// RGB565 palette (from the C64Esp32 project)
const uint16_t c64Colors[16] = {
    0x0000, 0xffff, 0x8000, 0xa7fc, 0xc218, 0x064a, 0x0014, 0xe74e,
    0xd42a, 0x6200, 0xfbae, 0x3186, 0x73ae, 0xa7ec, 0x043f, 0xb5d6};

} // namespace c64
