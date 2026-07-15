#pragma once
#include <Arduino.h>

// Subscribes to a notification topic (e.g. "dashboard/notify") so HA/Hubitat
// automations can push alerts straight to the screen without polling.
void mqtt_service_begin();
void mqtt_service_loop();
