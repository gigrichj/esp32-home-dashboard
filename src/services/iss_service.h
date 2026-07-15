#pragma once
#include <Arduino.h>

struct IssData {
  float lat = 0, lon = 0, altitudeKm = 0;
  uint32_t nextPassUnix = 0;
  int nextPassDurationSec = 0;
  bool valid = false;
};

extern IssData g_iss;

// N2YO "positions" + "visualpasses" endpoints for HOME_LAT/HOME_LON.
void iss_service_update();
