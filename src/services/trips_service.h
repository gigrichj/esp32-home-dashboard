#pragma once
#include <Arduino.h>
#include <time.h>

// One upcoming trip, pushed from the user's Mac via a local PUT request
// (see wifi_manager.cpp's /trips route) rather than fetched from the
// internet -- this is the one piece of dashboard content that comes from
// the user's own data, not a public API, so it's stored in NVS (via
// Preferences, same mechanism already used for WiFi credentials in
// wifi_manager.cpp, but its own separate namespace) instead of being
// re-fetched on a poll cadence.
struct Trip {
  String name;
  String company;     // e.g. cruise line / tour operator ("Viking")
  String location;
  String depart;      // "YYYY-MM-DD", kept as the original string for display
  String returnDate;  // "YYYY-MM-DD" -- named returnDate, not "return" (reserved word)
  // UTC midnight of the calendar date (day count * 86400) -- NOT local
  // midnight. Trip dates are pure calendar days with no real time-of-day
  // meaning, so they're stored timezone-independently (see
  // daysFromCivil() below) to avoid local-TZ/DST round-tripping bugs.
  // The countdown/ongoing-check logic compares against the device's
  // LOCAL calendar day (via daysFromCivil() on localtime() fields), not
  // this raw epoch value directly.
  time_t departUnix = 0;
  time_t returnUnix = 0;
};

// Manual, timezone-independent Y/M/D -> Unix days-since-epoch conversion
// (the well-known Howard Hinnant "days_from_civil" algorithm). Exposed
// here so screen_manager.cpp's countdown calculation can convert the
// device's LOCAL "now" (via localtime()) into the same day-count space
// as departUnix/returnUnix, without needing mktime()/localtime() on the
// stored UTC timestamps themselves.
int64_t daysFromCivil(int y, int m, int d);

static const int TRIPS_MAX = 20;
extern Trip g_trips[TRIPS_MAX];
extern int g_tripsCount;
extern bool g_tripsValid; // true once at least one successful load/save has happened

// Loads any previously-saved trip list from NVS at boot, so trips survive
// a reboot without needing a fresh push from the Mac. Returns true if a
// stored list was found and parsed successfully.
bool trips_service_begin();

// Parses and validates a JSON payload (see the format comment in the
// .cpp), then stores it both in memory (g_trips) and persistently in NVS.
// Returns false without changing any existing data if the payload is
// malformed or too large -- so a bad push can't wipe out a good list.
bool trips_service_save_json(const String& json);

// Index into g_trips of the "next" trip: the one with the soonest
// departUnix that hasn't fully passed yet (returnUnix still in the
// future). Returns -1 if there are no upcoming trips at all.
int trips_service_next_index();

// True while "now" falls between a trip's depart and return dates
// (inclusive) -- lets the draw code show "Currently traveling!" instead
// of a countdown for a trip that's already underway.
bool trips_service_is_ongoing(int index);
