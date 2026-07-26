#pragma once
#include <Arduino.h>

extern uint16_t* g_imageryPixels;
extern int g_imageryWidth;
extern int g_imageryHeight;
extern bool g_imageryValid;

// Decodes the first embedded image once into a fixed PSRAM pixel buffer.
// Call once from setup(), before the UI task starts drawing pages.
void imagery_init();

// Picks a different random embedded image and re-decodes it into the same
// buffer. Call periodically (every 15 minutes) from the network task.
void imagery_update();
