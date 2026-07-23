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
  float maxAz = 0;           // degrees, 0=N clockwise -- for drawing a compass needle
  String maxAzCompass = "";  // e.g. "NW" -- where to look at the pass's peak
};

static const int ISS_MAX_PASSES = 5;
extern IssPass g_issPasses[ISS_MAX_PASSES];
extern int g_issPassCount;

// Diagnostics for the visualpasses fetch specifically, separate from
// g_iss.lastHttpCode (which only reflects the /positions/ call) -- this
// endpoint silently returning maxEl=0 for every pass (chunked-encoding
// JSON parse failure, same root cause as the Open-Meteo/TLE fetches
// elsewhere in this project) was previously invisible on screen.
extern int g_issPassesLastHttpCode;
extern bool g_issPassesParseFailed;

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
// Index within g_issTrack representing "now" -- points before this
// index are the recent past (drawn dim), points at/after it are the
// near future (drawn bright).
static const int ISS_TRACK_NOW_INDEX = ISS_TRACK_POINTS / 2;
extern TrackPoint g_issTrack[ISS_TRACK_POINTS];
extern int g_issTrackCount;
extern bool g_issTrackValid;

// Last HTTP result from the TLE (orbital elements) fetch, and a human-
// readable reason if it failed -- the ground track can't draw without a
// successfully loaded TLE, so this gives on-screen visibility into why,
// without needing a serial monitor.
extern int g_tleLastHttpCode;
extern String g_tleLastFailureReason;

void iss_service_update();
