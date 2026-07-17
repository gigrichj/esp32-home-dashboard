#pragma once
#include <Arduino.h>

struct Aircraft {
  String icao;
  String callsign;
  float lat = 0, lon = 0;
  int altitudeFt = 0;
  int groundSpeedKt = 0;
  float trackDeg = 0;
  float bearingFromHome = 0;   // computed, for radar-scope plotting
  float distanceNm = 0;        // computed
};

static const int MAX_TRACKED_AIRCRAFT = 20;
extern Aircraft g_aircraft[MAX_TRACKED_AIRCRAFT];
extern int g_aircraftCount;

struct AviationStatus {
  int lastHttpCode = 0;
  String lastError = "";
};
extern AviationStatus g_aviationStatus;

// Pulls nearby aircraft from ADS-B Exchange (default) within a bounding
// box around HOME_LAT/HOME_LON, computes bearing/distance for each so
// the radar screen can plot them on a plain LVGL canvas (no map tiles).
void aviation_service_update();

// On-demand lookup by flight number / tail number.
bool aviation_lookup_flight(const String& flightNumber, Aircraft& out);
