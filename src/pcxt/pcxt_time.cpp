// pcxt_time.cpp - device implementation of the PIT real-time source (5 MHz).
//
// PIT8253.cpp calls FRC1Timer() to know how much real time has elapsed so IRQ0
// (the 18.2 Hz system timer) fires at wall-clock rate independent of how fast the
// 8086 interpreter runs. On the ESP32-S3 we derive a 5 MHz tick from the 1 MHz
// esp_timer microsecond clock. The host harness provides its own definition, so
// this translation unit is device-only.

#ifndef PCXT_HOST_BOOT

#include <stdint.h>

// ESP-IDF microsecond clock (declared here to avoid pulling esp_timer.h into the
// standalone compile-check; provided by the IDF at link time).
extern "C" int64_t esp_timer_get_time(void);

extern "C" void FRC1Timer_init(int /*prescaler*/) { }

extern "C" uint32_t FRC1Timer(void) {
  return (uint32_t)((uint64_t)esp_timer_get_time() * 5ULL);  // 5 ticks/us = 5 MHz
}

#endif // !PCXT_HOST_BOOT
