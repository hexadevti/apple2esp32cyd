// tiny386_roms.h — declarations for the SeaBIOS + VGABIOS images embedded in the firmware.
// The arrays are defined in tiny386_roms.c (auto-generated from the tiny386 release binaries).
// load_rom() in tiny386.cpp copies these into guest physical memory at boot.
#ifndef TINY386_ROMS_H
#define TINY386_ROMS_H

#ifdef __cplusplus
extern "C" {
#endif

extern const unsigned char seabios_rom[];
extern const unsigned int  seabios_rom_len;   // 131072 (128 KiB)
extern const unsigned char vgabios_rom[];
extern const unsigned int  vgabios_rom_len;   // 39936

#ifdef __cplusplus
}
#endif

#endif /* TINY386_ROMS_H */
