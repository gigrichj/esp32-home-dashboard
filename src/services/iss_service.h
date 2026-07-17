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

struct TrackPoint {
  float lat;
  float lon;
};

static const int ISS_TRACK_POINTS = 60;
extern TrackPoint g_issTrack[ISS_TRACK_POINTS];
extern int g_issTrackCount;
extern bool g_issTrackValid;

void iss_service_update();
