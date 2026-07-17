#pragma once
#include <Arduino.h>

static const int DEBUG_LOG_LINES = 14;
extern String g_debugLog[DEBUG_LOG_LINES];

// Appends a timestamped line to the on-screen debug ring buffer (visible
// on the DEBUG tab) and also prints it to Serial in case that's ever
// working. Use like printf: debug_log("weather HTTP %d", code);
void debug_log(const char* fmt, ...);
