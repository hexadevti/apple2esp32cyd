// msx_diskrom.h - embedded MSX disk-interface ROM (C-DISK), 16 KB in flash. See msx_diskrom.cpp.
#pragma once
#include <stdint.h>

extern const unsigned int  msxDiskRomLen;   // 16384
extern const unsigned char msxDiskRom[];     // C-DISK 16 KB disk-interface ROM (maps at $4000)
