#pragma once
// Panel: 800x480 RGB LCD via Waveshare ESP32-S3-Touch-LCD-7
// Touch: 5-point capacitive via I2C (GT911-family typical on this board)
//
// Pin mapping should be copied from Waveshare's official demo
// (Demo/Arduino/xxx_LVGL_Example) for rev 1.2 — pin assignments have
// changed slightly between board revisions, so confirm against your
// board's schematic before wiring this up.

#define SCREEN_WIDTH   800
#define SCREEN_HEIGHT  480

void display_init();   // sets up RGB panel driver, backlight, touch, and LVGL buffers
void display_loop();   // call from main loop if the panel driver needs periodic servicing
