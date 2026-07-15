#pragma once
#include <Arduino.h>

// Connects to WiFi and handles reconnection. Non-blocking after first call.
void wifi_manager_begin(const char* ssid, const char* password);
void wifi_manager_loop();
bool wifi_manager_is_connected();
