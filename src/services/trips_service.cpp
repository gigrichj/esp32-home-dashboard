#include "trips_service.h"
#include "../state_mutex.h"
#include <ArduinoJson.h>
#include <Preferences.h>

// Own separate Preferences namespace ("trips") -- kept entirely apart
// from wifi_manager.cpp's "dashboard" namespace (which holds WiFi
// credentials) so this feature can never collide with or accidentally
// overwrite WiFi settings.
static Preferences tripsPrefs;
static const char* TRIPS_NVS_KEY = "json";

// NVS practical ceiling for a single string value is commonly cited around
// 4000 bytes; capped well under that here so there's always headroom for
// NVS's own internal overhead, rather than pushing right up against the
// limit.
static const size_t TRIPS_JSON_MAX_BYTES = 3500;

Trip g_trips[TRIPS_MAX];
int g_tripsCount = 0;
bool g_tripsValid = false;

// Expected JSON format:
// {
//   "trips": [
//     { "name": "Rivers of India", "company": "Viking", "location": "India",
//       "depart": "2026-09-01", "return": "2026-09-25" },
//     ...
//   ]
// }
//
// "company" is optional (e.g. cruise line / tour operator) -- defaults to
// an empty string if omitted, same as name/location.
//
// Dates are plain "YYYY-MM-DD" strings -- deliberately not requiring the
// list to be pre-sorted; trips_service_next_index() below does the
// sorting/filtering work so the user can add trips in whatever order is
// convenient when editing the file locally.

// Manual, timezone-independent Y/M/D -> Unix days-since-epoch conversion
// (the well-known Howard Hinnant "days_from_civil" algorithm, public
// domain, exact for the proleptic Gregorian calendar). Used instead of
// mktime() below -- mktime() interprets/resolves the struct tm using
// the device's configured local timezone (EST5EDT, set via
// configTzTime() in main.cpp) and DST rules, which was causing trip
// dates to display exactly one day early: a "midnight local" struct tm
// passed through mktime()+localtime() round-tripped incorrectly on
// this platform's toolchain (this project has separately documented
// that timegm() isn't available here either -- a sign this toolchain's
// timezone-aware date functions have real quirks). Trip dates are pure
// calendar days with no meaningful time-of-day/timezone component, so
// treating them as UTC throughout (both here and in formatTripDate()
// in screen_manager.cpp, which uses gmtime() to match) sidesteps the
// whole local-TZ/DST ambiguity rather than fighting it.
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static bool parseDateToUnix(const String& dateStr, time_t& out) {
  int y, mo, d;
  if (sscanf(dateStr.c_str(), "%d-%d-%d", &y, &mo, &d) != 3) {
    return false;
  }
  if (y < 2000 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31) {
    return false;
  }
  int64_t days = daysFromCivil(y, mo, d);
  out = (time_t)(days * 86400LL);
  return true;
}

// Shared by both trips_service_begin() (loading previously-saved JSON
// from NVS) and trips_service_save_json() (a fresh push) -- parses into
// a local scratch array first, and only commits to the real g_trips[]
// if every step succeeds, so a malformed payload can never leave the
// in-memory state half-updated.
static bool parseAndApply(const String& json) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.printf("[Trips] JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonArray tripsArray = doc["trips"].as<JsonArray>();
  if (tripsArray.isNull()) {
    Serial.println("[Trips] JSON missing \"trips\" array");
    return false;
  }

  Trip scratch[TRIPS_MAX];
  int scratchCount = 0;

  for (JsonObject t : tripsArray) {
    if (scratchCount >= TRIPS_MAX) {
      Serial.printf("[Trips] more than %d trips in payload -- extras ignored\n", TRIPS_MAX);
      break;
    }
    const char* name = t["name"] | "";
    const char* company = t["company"] | "";
    const char* location = t["location"] | "";
    const char* depart = t["depart"] | "";
    const char* ret = t["return"] | "";

    time_t departUnix, returnUnix;
    if (!parseDateToUnix(depart, departUnix)) {
      Serial.printf("[Trips] skipping trip '%s' -- bad depart date '%s'\n", name, depart);
      continue;
    }
    if (!parseDateToUnix(ret, returnUnix)) {
      Serial.printf("[Trips] skipping trip '%s' -- bad return date '%s'\n", name, ret);
      continue;
    }

    scratch[scratchCount].name = name;
    scratch[scratchCount].company = company;
    scratch[scratchCount].location = location;
    scratch[scratchCount].depart = depart;
    scratch[scratchCount].returnDate = ret;
    scratch[scratchCount].departUnix = departUnix;
    scratch[scratchCount].returnUnix = returnUnix;
    scratchCount++;
  }

  // Commit -- only reached if every step above succeeded without a fatal
  // parse error (individual bad trips are skipped above, not fatal).
  // g_trips is read by the UI task while drawing (screen_manager.cpp's
  // draw_world_trips(), guarded there by StateLockGuard) and written here
  // by the network task (via the /trips WebServer handler) -- same
  // cross-core sharing pattern as every other service, so the actual
  // copy is protected the same way, kept as brief as possible.
  state_lock();
  for (int i = 0; i < scratchCount; i++) {
    g_trips[i] = scratch[i];
  }
  g_tripsCount = scratchCount;
  g_tripsValid = true;
  state_unlock();
  return true;
}

bool trips_service_begin() {
  tripsPrefs.begin("trips", false);
  String stored = tripsPrefs.getString(TRIPS_NVS_KEY, "");
  if (stored.length() == 0) {
    Serial.println("[Trips] no stored trips found in NVS");
    return false;
  }
  bool ok = parseAndApply(stored);
  if (ok) {
    Serial.printf("[Trips] loaded %d trip(s) from NVS\n", g_tripsCount);
  } else {
    Serial.println("[Trips] stored NVS trips JSON failed to parse -- ignoring");
  }
  return ok;
}

bool trips_service_save_json(const String& json) {
  if (json.length() == 0) {
    Serial.println("[Trips] rejected: empty payload");
    return false;
  }
  if (json.length() > TRIPS_JSON_MAX_BYTES) {
    Serial.printf("[Trips] rejected: payload %u bytes exceeds %u byte cap\n",
                  (unsigned)json.length(), (unsigned)TRIPS_JSON_MAX_BYTES);
    return false;
  }
  if (!parseAndApply(json)) {
    return false; // existing g_trips[] left untouched on failure
  }
  tripsPrefs.putString(TRIPS_NVS_KEY, json);
  Serial.printf("[Trips] saved %d trip(s) to NVS\n", g_tripsCount);
  return true;
}

int trips_service_next_index() {
  if (!g_tripsValid || g_tripsCount == 0) {
    return -1;
  }
  time_t now = time(nullptr);
  int bestIdx = -1;
  time_t bestDepart = 0;
  for (int i = 0; i < g_tripsCount; i++) {
    // A trip still counts as "upcoming" even if it's already started, as
    // long as it hasn't ended yet -- that's what lets an ongoing trip
    // still show up as "next" (see trips_service_is_ongoing()) instead
    // of disappearing the moment departure day begins.
    if (g_trips[i].returnUnix < now) {
      continue; // fully in the past
    }
    if (bestIdx == -1 || g_trips[i].departUnix < bestDepart) {
      bestIdx = i;
      bestDepart = g_trips[i].departUnix;
    }
  }
  return bestIdx;
}

bool trips_service_is_ongoing(int index) {
  if (index < 0 || index >= g_tripsCount) {
    return false;
  }
  time_t now = time(nullptr);
  return now >= g_trips[index].departUnix && now <= g_trips[index].returnUnix;
}
