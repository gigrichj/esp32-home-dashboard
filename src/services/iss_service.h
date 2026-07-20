#pragma once
#include <Arduino.h>

struct IssData {
  float lat = 0, lon = 0, altitudeKm = 0;
  uint32_t nextPassUnix = 0;
  int nextPassDurationSec = 0;
  bool valid = false;
  int lastHttpCode = -999;
};

extern IssData g_iss;

struct IssPass {
  uint32_t startUnix = 0;
  uint32_t endUnix = 0;
  int maxElevationDeg = 0;
  float magnitude = 99.0f;
  String maxAzCompass = "";  // e.g. "NW" -- where to look at the pass's peak
};

static const int ISS_MAX_PASSES = 5;
extern IssPass g_issPasses[ISS_MAX_PASSES];
extern int g_issPassCount;

// Number of people currently aboard the ISS specifically (Open Notify's
// astros.json lists everyone in space across all craft; we filter to ISS).
extern int g_issCrewCount;

// Last HTTP result from the crew-count fetch (-999 = never attempted),
// so a stuck-at-zero count can be diagnosed on-screen.
extern int g_issCrewLastHttpCode;

struct TrackPoint {
  float lat;
  float lon;
};

static const int ISS_TRACK_POINTS = 60;
extern TrackPoint g_issTrack[ISS_TRACK_POINTS];
extern int g_issTrackCount;
extern bool g_issTrackValid;

void iss_service_update();
