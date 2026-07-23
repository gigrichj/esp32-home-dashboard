#pragma once
#include <Arduino.h>

// Runtime-adjustable aviation poll interval (ms), for empirically testing
// whether flicker correlates with poll frequency. No reboot needed to
// change this - unlike the RGB bounce buffer, it's just a timing value.
extern uint32_t g_aviationPollMs;

// Cycles 15s -> 30s -> 45s -> 60s -> back to 15s.
void cycleAviationPollInterval();

// Why the last reset happened (panic, task watchdog, brownout,
// power-on, etc.), read from ESP-IDF's esp_reset_reason() at boot.
// Shown on the Debug page so an actual crash can be diagnosed from
// what's on screen instead of guessed at -- serial monitor is
// unreliable on this board's native USB CDC, so this is the real
// diagnostic channel.
extern const char* g_resetReasonStr;
void debug_controls_record_reset_reason(); // call once, early in setup()

// Lowest free-heap value seen since boot. A steadily dropping watermark
// right up to a crash points to a slow leak; a stable watermark right
// up to a crash points elsewhere (brownout, stack overflow, a one-off
// blocking call).
extern size_t g_minFreeHeapSeen;
void debug_controls_update_min_heap(); // cheap, call every loop iteration
