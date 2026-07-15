#pragma once
#include <Arduino.h>

struct DeviceState {
  String name;
  String type;   // "light", "lock", "garage", "thermostat", "scene"
  String state;  // "on"/"off"/"locked"/"open"/etc, or a numeric temp as string
};

static const int MAX_DEVICES = 30;
extern DeviceState g_devices[MAX_DEVICES];
extern int g_deviceCount;

void smarthome_service_update();

// Command helpers — both hit local LAN endpoints, no cloud round-trip.
bool hubitat_send_command(const String& deviceId, const String& command);
bool ha_call_service(const String& domain, const String& service, const String& entityId);
