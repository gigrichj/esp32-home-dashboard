#pragma once
#include <Arduino.h>

// WiFi credentials are stored in NVS (via Preferences) rather than only
// compiled into secrets.h, so the network can be changed without
// reflashing. If no saved/working credentials are found, this opens a
// setup hotspot ("ESP32-Dashboard-Setup") with a web form at
// http://192.168.4.1 to enter new ones. Once connected normally, the
// device is also reachable at http://dashboard.local with a small status
// page that has a "Reconfigure WiFi" button to re-enter setup mode later
// without touching the physical device at all.

void wifi_manager_begin();
void wifi_manager_loop();
bool wifi_manager_is_connected();

// True while showing the setup hotspot/portal instead of being on the
// home network — main.cpp uses this to show a setup screen instead of
// the normal dashboard.
bool wifi_manager_in_setup_mode();
