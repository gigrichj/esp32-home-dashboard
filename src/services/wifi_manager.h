#pragma once
#include <Arduino.h>

void wifi_manager_begin();
void wifi_manager_loop();
bool wifi_manager_is_connected();

bool wifi_manager_in_setup_mode();

String wifi_manager_last_attempted_ssid();
int wifi_manager_last_status_code();

// Cached signal strength, refreshed about once a second on the network
// task (Core 0). Draw code should read this instead of calling
// WiFi.RSSI() directly -- that call was happening every ~200ms draw
// frame from inside the Dashboard and Debug pages, on the UI task
// (Core 1), and the WiFi radio's own internal timing (RSSI is only
// refreshed on its beacon/probe cadence, roughly once a second) meant
// that call could occasionally block the render thread right as the
// radio's internal state was mid-update -- a likely source of the
// steady ~1s flicker pulse. -100 means no reading yet / disconnected.
extern int g_wifiRssi;
