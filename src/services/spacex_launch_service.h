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
