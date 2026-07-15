#pragma once

// Builds the tabview and all child screens (dashboard, aviation, porsche,
// smarthome, cameras, iss, weather, calendar) and wires their LVGL widgets
// to the service data structs (g_weather, g_aircraft, g_devices, etc).
void screen_manager_init();

// Called periodically (or via LVGL timers) to refresh widget contents
// from the latest service data without rebuilding the whole screen.
void screen_manager_refresh();
