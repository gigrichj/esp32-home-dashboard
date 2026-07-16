#pragma once
#include <stdint.h>

// Builds and draws all dashboard "screens" using the raw-framebuffer
// Canvas API (src/panel_display.h) rather than LVGL. Screens are plain
// functions that draw into PanelDisplay::screen; screen_manager_draw()
// picks which one runs based on the current tab, and the caller is
// responsible for calling PanelDisplay::screen.present() afterward.

void screen_manager_init();

// Draws the current tab's content (does not call present() itself).
void screen_manager_draw();

// Call once per loop with the latest touch state; short taps cycle tabs.
void screen_manager_handle_touch(bool touched, uint16_t x, uint16_t y);
