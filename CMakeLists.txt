# CMakeLists.txt — DESKTOP (SDL2) debug build of emu6502. Does NOT touch the arduino-cli device
# build (that path never invokes CMake and never defines BOARD_DESKTOP). See desktop/README.md.
#
#   Windows (native .exe, gdb):   MSYS2 MinGW32 shell ->
#     cmake -G "MinGW Makefiles" -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
#   WSL/Linux (sanitizers rail):
#     cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DEMU_SANITIZE=thread && cmake --build build
cmake_minimum_required(VERSION 3.16)
project(emu6502_desktop C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)   # debug tool -> default to a debuggable build
endif()
set(CMAKE_CXX_FLAGS_DEBUG "-Og -g3 -fno-omit-frame-pointer")

set(REPO ${CMAKE_CURRENT_SOURCE_DIR})

# Match the ESP32: `char` is effectively unsigned there, and the cores read memory bytes into `char`
# and do arithmetic on them (e.g. Apple II text glyph index `chr*7*8` for codes >= 0x80). x86/MinGW
# defaults `char` to SIGNED, which wraps those into out-of-bounds reads (garbage text). -funsigned-char
# makes the desktop behave like the device.
add_compile_options(-funsigned-char)

# --- ABI: 32-bit (ILP32) so long/pointer widths match the ESP32 (Xtensa LX). Toggle off only if you
#     accept the fidelity gap. SDL2 + the C++ runtime must then also be 32-bit. ---
option(EMU_M32 "Build 32-bit (-m32) to match ESP32 ILP32" ON)
if(EMU_M32)
  add_compile_options(-m32)
  add_link_options(-m32)
endif()

# --- Sanitizers (WSL/Linux clang/gcc rail): -DEMU_SANITIZE=address,undefined  or  thread ---
if(EMU_SANITIZE)
  add_compile_options(-fsanitize=${EMU_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${EMU_SANITIZE})
endif()

# --- Sources: the SAME core/shared code the device compiles, minus the hardware-only leaves
#     (replaced by src/desktop/*), minus PC-XT/fabgl (Phase 3) and the IIGS bring-up harnesses. ---
file(GLOB_RECURSE CORE_SRC CONFIGURE_DEPENDS
  ${REPO}/src/apple2/*.cpp ${REPO}/src/c64/*.cpp  ${REPO}/src/nes/*.cpp
  ${REPO}/src/atari/*.cpp  ${REPO}/src/iigs/*.cpp ${REPO}/src/msx/*.cpp
  ${REPO}/src/sms/*.cpp    ${REPO}/src/z80/*.cpp
  ${REPO}/src/shared/*.cpp)
list(REMOVE_ITEM CORE_SRC
  ${REPO}/src/shared/audio_amp.cpp      # -> src/desktop/audio_sdl.cpp
  ${REPO}/src/shared/speaker.cpp        # -> src/desktop/audio_sdl.cpp
  ${REPO}/src/shared/display_gfx.cpp    # -> src/desktop/display_sdl.cpp
  ${REPO}/src/shared/sd.cpp             # -> src/desktop/sd_host.cpp
  ${REPO}/src/shared/usbgamepad.cpp     # ESP32-S3 USB host driver (input via SDL instead)
  ${REPO}/src/shared/keyboardPs2.cpp    # CYD PS/2 path (input via SDL instead)
  ${REPO}/src/iigs/m0_bench.cpp         # bring-up harness (compiled only with -DIIGS_M0_BENCH)
  ${REPO}/src/iigs/cpu65816_test.cpp)   # bring-up harness

file(GLOB DESKTOP_SRC CONFIGURE_DEPENDS ${REPO}/src/desktop/*.cpp)

# tiny386 (Intel i386 PC emulator): vendored C99 core (pc/i386/vga/ide/...) + the C++ glue
# (tiny386.cpp) + embedded BIOS arrays (tiny386_roms.c). Compiled into the desktop build so the
# core can be debugged with gdb; the device build compiles the same files via arduino-cli under src/.
file(GLOB TINY386_SRC CONFIGURE_DEPENDS ${REPO}/src/tiny386/*.c ${REPO}/src/tiny386/*.cpp)

# Compile the Arduino sketch as a C++ translation unit (copied so the compiler accepts the name).
configure_file(${REPO}/emu6502.ino ${CMAKE_BINARY_DIR}/emu6502_ino.cpp COPYONLY)

add_executable(emu6502
  ${CMAKE_BINARY_DIR}/emu6502_ino.cpp
  ${REPO}/globals.cpp
  ${CORE_SRC}
  ${DESKTOP_SRC}
  ${TINY386_SRC})

# arduino_shim MUST come first so #include <Arduino.h>/SD.h/EEPROM.h/SPI.h/FS.h/BLEDevice.h and
# "driver/i2s.h" etc. resolve to the shim instead of the real (absent) libraries.
target_include_directories(emu6502 BEFORE PRIVATE
  ${REPO}/src/desktop/arduino_shim
  ${REPO})
# tiny386 vendored sources include their own headers by bare name ("pc.h", ...) -> put their dir on
# the include path. AFTER the shim/repo dirs so e.g. emu.h still resolves to the project's, not here.
target_include_directories(emu6502 PRIVATE ${REPO}/src/tiny386)

target_compile_definitions(emu6502 PRIVATE BOARD_DESKTOP)
# The codebase is full of Arduino-isms; keep it building (warnings on, not fatal).
target_compile_options(emu6502 PRIVATE -Wno-write-strings -Wno-narrowing)

# --- SDL2 + threads. We use SDL_MAIN_HANDLED (main.cpp), so SDL2main is NOT needed (avoids the
#     MinGW SDL2main/SDL2 link-order trap). ---
find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(emu6502 PRIVATE Threads::Threads)
if(TARGET SDL2::SDL2)
  target_link_libraries(emu6502 PRIVATE SDL2::SDL2)
else()
  target_include_directories(emu6502 PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(emu6502 PRIVATE ${SDL2_LIBRARIES})
endif()

# Copy the MinGW + SDL2 runtime DLLs next to emu6502.exe so it runs standalone (double-click / F5)
# WITHOUT needing C:\msys64\mingw32\bin on PATH. (libgcc may be _dw2 or _sjlj depending on the build.)
if(WIN32)
  get_filename_component(MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
  foreach(dll SDL2.dll libstdc++-6.dll libwinpthread-1.dll
              libgcc_s_dw2-1.dll libgcc_s_sjlj-1.dll libgcc_s_seh-1.dll)
    if(EXISTS "${MINGW_BIN}/${dll}")
      add_custom_command(TARGET emu6502 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MINGW_BIN}/${dll}" "$<TARGET_FILE_DIR:emu6502>"
        VERBATIM)
    endif()
  endforeach()
endif()
