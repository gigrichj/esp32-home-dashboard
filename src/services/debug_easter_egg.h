#pragma once
#include <Arduino.h>

extern uint16_t* g_debugEasterEggPixels;
extern int g_debugEasterEggWidth;
extern int g_debugEasterEggHeight;
extern bool g_debugEasterEggValid;

// Decodes the embedded easter-egg JPEG once into a PSRAM pixel buffer.
// Call once from setup(), before the UI task starts drawing pages.
void debug_easter_egg_init();
