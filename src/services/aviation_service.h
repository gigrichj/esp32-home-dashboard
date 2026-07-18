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
  int verticalRateFpm = 0;
  String squawk;
};

static const int MAX_TRACKED_AIRCRAFT = 20;
extern Aircraft g_aircraft[MAX_TRACKED_AIRCRAFT];
extern int g_aircraftCount;

struct AviationStatus {
  int lastHttpCode = 0;
  String lastError = "";
};
extern AviationStatus g_aviationStatus;

// Result of tapping an aircraft: type/photo/route looked up from adsbdb.com.
struct AircraftDetail {
  bool valid = false;
  bool lookupInProgress = false;
  String lookedUpIcao;   // which aircraft this belongs to, for cache checks
  String type;
  String photoThumbUrl;  // used in Phase 2 (image rendering)
  String originName;
  String originIata;
  String destName;
  String destIata;
  String lookupError;
};
extern AircraftDetail g_aircraftDetail;

// Decoded aircraft photo, allocated in PSRAM. Valid only when
// g_aircraftPhotoValid is true; owned entirely by aviation_service.cpp.
extern uint16_t* g_aircraftPhotoPixels;
extern int g_aircraftPhotoWidth;
extern int g_aircraftPhotoHeight;
extern bool g_aircraftPhotoValid;

void aviation_service_update();
bool aviation_lookup_flight(const String& flightNumber, Aircraft& out);

// Called from the UI (tap on an aircraft) - queues a detail lookup for
// networkTask to service. Safe to call repeatedly; no-ops if the same
// aircraft was already looked up or a lookup is already in flight.
void aviation_request_detail(const String& icaoHex, const String& callsign);

// Called every networkTask loop iteration - services any pending
// detail request. Cheap no-op when nothing is pending.
void aviation_service_detail_loop();
