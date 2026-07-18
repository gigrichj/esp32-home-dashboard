#pragma once
#include <Arduino.h>

// Runtime-adjustable aviation poll interval (ms), for empirically testing
// whether flicker correlates with poll frequency. No reboot needed to
// change this - unlike the RGB bounce buffer, it's just a timing value.
extern uint32_t g_aviationPollMs;

// Cycles 15s -> 30s -> 45s -> 60s -> back to 15s.
void cycleAviationPollInterval();
