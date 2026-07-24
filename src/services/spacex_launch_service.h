#pragma once
#include <Arduino.h>

// One upcoming SpaceX launch, from Launch Library 2 (thespacedevs.com).
struct SpacexLaunch {
  String displayName;   // LL2's top-level "name", e.g. "Falcon 9 Block 5 | Starlink Group 17-51"
  String rocketName;    // short config name, e.g. "Falcon 9", "Falcon Heavy", "Starship"
  String missionName;   // mission name alone, e.g. "Starlink Group 17-51"
  String padName;       // e.g. "Space Launch Complex 40"
  String locationName;  // e.g. "Cape Canaveral SFS, FL, USA"
  String statusName;    // e.g. "Go", "TBD", "Hold", "Success"
  uint32_t netUnix = 0;  // launch time (NET = "no earlier than"), unix timestamp UTC
  String imageUrl;      // generic launch/rocket photo URL from LL2 --
                        // not necessarily a distinct mission patch image.
  String launchId;      // LL2 launch UUID, needed for the separate
                        // landing-detail fetch (only available via the
                        // single-launch detail endpoint, not this list).
};

static const int SPACEX_MAX_LAUNCHES = 12;
extern SpacexLaunch g_spacexLaunches[SPACEX_MAX_LAUNCHES];
extern int g_spacexLaunchCount;
extern bool g_spacexValid;
extern int g_spacexLastHttpCode;

// Fetches upcoming SpaceX launches from Launch Library 2, filters
// client-side to the next 30 days, and stores them (already in ascending
// launch-time order from the API) into g_spacexLaunches.
void spacex_launch_service_update();

// Decoded photo for the NEXT launch only (index 0 of g_spacexLaunches) --
// mirrors aviation_service.cpp's fetchAndDecodePhoto() pattern exactly.
// Only the next launch is decoded (not all 12) to keep PSRAM/bandwidth
// usage reasonable -- JPEG decode buffers can be sizeable.
extern uint16_t* g_spacexImagePixels;
extern int g_spacexImageWidth;
extern int g_spacexImageHeight;
extern bool g_spacexImageValid;
void spacex_fetch_next_image();

// Booster landing info for the NEXT launch only -- only available via a
// separate, much heavier single-launch detail fetch (LL2's list/upcoming
// endpoint doesn't include it), so scoped to just the next launch rather
// than all 12 to keep that extra fetch's payload size reasonable.
extern bool g_spacexLandingValid;
extern bool g_spacexLandingAttempt;
extern String g_spacexLandingLocation;
extern String g_spacexLandingAbbrev;
extern String g_spacexLandingType;
void spacex_fetch_next_landing_info();
