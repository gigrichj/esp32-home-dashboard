#include "iss_service.h"
#include <WiFiClient.h>
#include <NetworkClientSecure.h>
#include <esp_heap_caps.h>
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Sgp4.h>
#include <time.h>
#include "../state_mutex.h"

IssData g_iss;
IssPass g_issPasses[ISS_MAX_PASSES];
int g_issPassCount = 0;
int g_issPassesLastHttpCode = -999;
bool g_issPassesParseFailed = false;
int g_issCrewCount = 0;
int g_issCrewLastHttpCode = -999;
TrackPoint g_issTrack[ISS_TRACK_POINTS];
int g_issTrackCount = 0;
bool g_issTrackValid = false;

static Sgp4 sat;
static bool tleLoaded = false;
int g_tleLastHttpCode = -999;
String g_tleLastFailureReason = "";
static uint32_t lastTleFetchMs = 0;
static const uint32_t TLE_REFRESH_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL; // 6 hours

// Crew count is decoupled from the TLE refresh -- poll every 60s until we
// have a first successful value, then settle into the same 6-hour cadence
// as the TLE (crew rotations are infrequent, no need to check more often).
static bool crewCountLoaded = false;
static uint32_t lastCrewFetchMs = 0;
static const uint32_t CREW_REFRESH_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL; // 6 hours
static const uint32_t CREW_REFRESH_RETRY_MS = 60UL * 1000UL; // 60 seconds, until first success
static const int TRACK_STEP_SECONDS = 100; // 60 pts * 100s = 6000s (~100min), > one ISS orbit (~92min)

// Fetches the current TLE from CelesTrak (free, no key needed) and loads it
// into the SGP4 propagator. TLEs don't change fast, so this only needs to
// run every few hours; the ground-track math itself is pure local
// computation with zero network cost once a TLE is loaded.
static void fetchCrewCount() {
  // Deliberately bare-bones -- this worked fine before an earlier "fix"
  // (timeout/redirects/User-Agent, aimed at a 7Timer-style 302 issue that
  // didn't actually apply here) broke it. Reverted to the simple version.
  HTTPClient http;
  http.begin("http://api.open-notify.org/astros.json");
  int code = http.GET();
  state_lock();
  g_issCrewLastHttpCode = code;
  state_unlock();
  if (code == 200) {
    String payload = http.getString(); // network I/O -- stays outside the lock
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      JsonArray people = doc["people"].as<JsonArray>();
      int count = 0;
      for (JsonObject p : people) {
        const char* craft = p["craft"] | "";
        if (String(craft) == "ISS") count++;
      }
      state_lock();
      g_issCrewCount = count;
      state_unlock();
      crewCountLoaded = true;
    }
  } else {
    Serial.printf("[ISS] astros.json HTTP %d\n", code);
  }
  http.end();
}

// Reads an HTTP response body with an explicit timeout and a yield on
// every iteration, instead of http.getString() -- the same fix applied to
// aviation (v128) and astro (v133/v141) after connection resets were found
// to interact badly with that call. Shared by both TLE sources below.
static bool readTleBodySafely(HTTPClient& http, String& outPayload) {
  int payloadLen = http.getSize();
  int bufSize = (payloadLen > 0) ? payloadLen + 1 : 8192;
  char *rawBuf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
  if (rawBuf == nullptr) {
    Serial.println("[ISS] TLE payload buffer alloc failed");
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t readTotal = 0;
  uint32_t startMs = millis();
  bool readError = false;
  while (readTotal < (size_t)(bufSize - 1) && millis() - startMs < 15000) {
    if (!http.connected() && stream->available() == 0) break;
    size_t avail = stream->available();
    if (avail > 0) {
      int toRead = (int)min(avail, (size_t)(bufSize - 1 - readTotal));
      int r = stream->readBytes(rawBuf + readTotal, toRead);
      if (r <= 0) { readError = true; break; }
      readTotal += r;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5)); // yield -- the critical fix
    }
  }
  rawBuf[readTotal] = '\0';

  if (readError) {
    Serial.println("[ISS] TLE payload read error");
    free(rawBuf);
    return false;
  }

  outPayload = String(rawBuf);
  free(rawBuf);
  return true;
}

// Common finish step once we have a name + two TLE lines from either source.
static bool initSatFromTle(const String& nameLine, const String& line1, const String& line2) {
  if (line1.length() < 60 || line2.length() < 60) {
    Serial.println("[ISS] TLE lines look truncated, skipping");
    state_lock();
    g_tleLastFailureReason = "lines truncated (l1=" + String(line1.length()) + " l2=" + String(line2.length()) + ")";
    state_unlock();
    return false;
  }

  char nameBuf[25];
  char line1Buf[130];
  char line2Buf[130];
  strlcpy(nameBuf, nameLine.c_str(), sizeof(nameBuf));
  strlcpy(line1Buf, line1.c_str(), sizeof(line1Buf));
  strlcpy(line2Buf, line2.c_str(), sizeof(line2Buf));

  sat.site((double)HOME_LAT, (double)HOME_LON, 0);
  sat.init(nameBuf, line1Buf, line2Buf); // return value just means "TLE unchanged since last call" - not an error
  Serial.println("[ISS] TLE loaded/refreshed");
  state_lock();
  g_tleLastFailureReason = "";
  state_unlock();
  return true;
}

static bool fetchTLEFromCelestrak() {
  HTTPClient http;
  http.begin("https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE");
  // Force HTTP/1.0 to avoid chunked transfer encoding -- our manual
  // raw-stream read loop doesn't strip chunk-size framing, which caused
  // "invalid input" JSON parse errors on Open-Meteo earlier tonight (v141)
  // even for small responses; applying the same fix here defensively.
  http.useHTTP10(true);
  int code = http.GET();
  state_lock();
  g_tleLastHttpCode = code;
  state_unlock();
  if (code != 200) {
    Serial.printf("[ISS] Celestrak TLE fetch HTTP %d\n", code);
    state_lock();
    g_tleLastFailureReason = "Celestrak: HTTP " + String(code);
    state_unlock();
    http.end();
    return false;
  }

  String payload;
  if (!readTleBodySafely(http, payload)) {
    state_lock();
    g_tleLastFailureReason = "Celestrak: payload read error";
    state_unlock();
    http.end();
    return false;
  }
  http.end();

  int nl1 = payload.indexOf('\n');
  int nl2 = payload.indexOf('\n', nl1 + 1);
  if (nl1 < 0 || nl2 < 0) {
    Serial.println("[ISS] Celestrak TLE response missing expected line breaks");
    state_lock();
    g_tleLastFailureReason = "Celestrak: response missing line breaks (len=" + String(payload.length()) + ")";
    state_unlock();
    return false;
  }

  String nameLine = payload.substring(0, nl1);
  String line1 = payload.substring(nl1 + 1, nl2);
  String line2 = payload.substring(nl2 + 1);
  nameLine.trim();
  line1.trim();
  line2.trim();

  return initSatFromTle(nameLine, line1, line2);
}

// Fallback used only when Celestrak is unreachable. tle.ivanstanojevic.me
// (free, no auth) caches CelesTrak's own data daily and re-serves it as
// JSON from separate infrastructure -- a genuinely independent point of
// failure from Celestrak itself, so it stays up during a Celestrak outage
// like the one that prompted adding this (confirmed live via direct browser
// test on 2026-07-21).
static bool fetchTLEFromIvanstanojevic() {
  HTTPClient http;
  http.begin("https://tle.ivanstanojevic.me/api/tle/25544");
  // Force HTTP/1.0 to avoid chunked transfer encoding -- same fix as
  // Open-Meteo (v141); transfer encoding is a server choice independent
  // of response size, so even this small JSON response can arrive chunked
  // and get corrupted by our manual read loop, producing "invalid input".
  http.useHTTP10(true);
  int code = http.GET();
  state_lock();
  g_tleLastHttpCode = code;
  state_unlock();
  if (code != 200) {
    Serial.printf("[ISS] tle.ivanstanojevic.me fetch HTTP %d\n", code);
    state_lock();
    g_tleLastFailureReason = "ivanstanojevic: HTTP " + String(code);
    state_unlock();
    http.end();
    return false;
  }

  String payload;
  if (!readTleBodySafely(http, payload)) {
    state_lock();
    g_tleLastFailureReason = "ivanstanojevic: payload read error";
    state_unlock();
    http.end();
    return false;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[ISS] tle.ivanstanojevic.me JSON parse error: %s\n", err.c_str());
    state_lock();
    g_tleLastFailureReason = "ivanstanojevic: JSON parse: " + String(err.c_str());
    state_unlock();
    return false;
  }

  const char* name = doc["name"] | "ISS (ZARYA)";
  const char* line1 = doc["line1"] | "";
  const char* line2 = doc["line2"] | "";
  if (strlen(line1) == 0 || strlen(line2) == 0) {
    Serial.println("[ISS] tle.ivanstanojevic.me response missing line1/line2");
    state_lock();
    g_tleLastFailureReason = "ivanstanojevic: response missing line1/line2";
    state_unlock();
    return false;
  }

  return initSatFromTle(String(name), String(line1), String(line2));
}

static bool fetchAndInitTLE() {
  if (fetchTLEFromCelestrak()) {
    return true;
  }

  Serial.println("[ISS] Celestrak failed, falling back to tle.ivanstanojevic.me");
  return fetchTLEFromIvanstanojevic();
}

static void computeGroundTrack() {
  if (!tleLoaded) {
    state_lock();
    g_issTrackValid = false;
    state_unlock();
    return;
  }

  uint32_t nowUnix = (uint32_t)time(nullptr);
  if (nowUnix < 100000) {
    state_lock();
    g_issTrackValid = false; // clock not synced yet
    state_unlock();
    return;
  }

  // Spans both directions in time around "now" -- indices before
  // ISS_TRACK_NOW_INDEX are the recent past, indices at/after it are the
  // near future. offsetSteps is negative for past points, zero at "now",
  // positive for future points.
  //
  // Locked for the whole loop -- g_issTrackCount and g_issTrack[] must
  // never be visible to a reader in a state where the count implies more
  // points than have actually been written yet.
  state_lock();
  g_issTrackCount = 0;
  for (int i = 0; i < ISS_TRACK_POINTS; i++) {
    long offsetSteps = (long)i - (long)ISS_TRACK_NOW_INDEX;
    unsigned long t = (unsigned long)nowUnix + (unsigned long)(offsetSteps * TRACK_STEP_SECONDS);
    sat.findsat(t);
    g_issTrack[g_issTrackCount].lat = sat.satLat;
    g_issTrack[g_issTrackCount].lon = sat.satLon;
    g_issTrackCount++;
  }
  g_issTrackValid = true;
  state_unlock();
}

void iss_service_update() {
  if (!wifi_manager_is_connected()) return;

  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
    "https://api.n2yo.com/rest/v1/satellite/positions/25544/%f/%f/0/1/&apiKey=%s",
    (double)HOME_LAT, (double)HOME_LON, N2YO_API_KEY);

  // Confirmed on-device: this fetch was failing from heap FRAGMENTATION,
  // not exhaustion -- a real log showed 125528 bytes total free internal
  // heap but only a 40948-byte largest contiguous block. TLS handshakes
  // need one large contiguous allocation for session buffers; the
  // default NetworkClientSecure RX/TX buffer sizes (commonly ~16KB each,
  // 32KB+ combined plus handshake overhead) don't fit in a 41KB largest
  // block once other allocations elsewhere have fragmented the heap.
  // Fix: create the TLS client explicitly and shrink its buffers via
  // setBufferSizes() before connecting, so the handshake needs less
  // contiguous space to succeed. setInsecure() preserves the same no-
  // certificate-validation behavior the old bare http.begin(url) had.
  NetworkClientSecure client;
  client.setInsecure();
  client.setBufferSizes(4096, 1024);

  Serial.printf("[ISS] free internal heap before HTTPS: %u bytes, largest free block: %u bytes\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  http.begin(client, url);
  int code = http.GET();
  state_lock();
  g_iss.lastHttpCode = code;
  state_unlock();
  if (code == 200) {
    String payload = http.getString(); // network I/O -- stays outside the lock
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      Serial.printf("[ISS] JSON parse error: %s\n", err.c_str());
      Serial.printf("[ISS] raw payload: %s\n", payload.c_str());
    } else {
      JsonObject pos = doc["positions"][0];
      state_lock();
      g_iss.lat        = pos["satlatitude"]  | 0.0f;
      g_iss.lon        = pos["satlongitude"] | 0.0f;
      g_iss.altitudeKm = pos["sataltitude"]  | 0.0f;
      g_iss.valid = true;
      state_unlock();
    }
  } else {
    Serial.printf("[ISS] HTTP %d\n", code);
  }
  http.end();

  HTTPClient passHttp;
  char passUrl[256];
  snprintf(passUrl, sizeof(passUrl),
    "https://api.n2yo.com/rest/v1/satellite/visualpasses/25544/%f/%f/0/7/300/&apiKey=%s",
    (double)HOME_LAT, (double)HOME_LON, N2YO_API_KEY);

  passHttp.begin(passUrl);
  // N2YO's visualpasses response was silently failing to parse -- traced
  // to chunked transfer encoding, the same root cause hit twice before
  // with Open-Meteo (astro fallback, ISS TLE fallback). Forcing HTTP/1.0
  // gets a plain Content-Length body instead, which plain getString()
  // below expects.
  passHttp.useHTTP10(true);
  int passCode = passHttp.GET();
  state_lock();
  g_issPassesLastHttpCode = passCode;
  g_issPassesParseFailed = false;
  state_unlock();
  if (passCode == 200) {
    String passPayload = passHttp.getString(); // network I/O -- stays outside the lock
    JsonDocument passDoc;
    DeserializationError passErr = deserializeJson(passDoc, passPayload);
    if (passErr) {
      state_lock();
      g_issPassesParseFailed = true;
      state_unlock();
      Serial.printf("[ISS] visualpasses JSON parse error: %s\n", passErr.c_str());
    } else {
      JsonArray passes = passDoc["passes"].as<JsonArray>();
      // Locked for the whole loop (plus the nextPass fields that depend
      // on g_issPasses[0]) -- g_issPassCount and g_issPasses[] must never
      // be visible to a reader in a state where the count implies more
      // entries than have actually been written yet.
      state_lock();
      g_issPassCount = 0;
      for (JsonObject p : passes) {
        if (g_issPassCount >= ISS_MAX_PASSES) break;
        g_issPasses[g_issPassCount].startUnix       = p["startUTC"] | 0;
        g_issPasses[g_issPassCount].endUnix         = p["endUTC"]   | 0;
        // N2YO returns maxEl as a float (e.g. 23.08), but the struct field
        // is an int -- requesting it directly with an int default (`| 0`)
        // was silently returning the default every time instead of
        // converting, since the JSON value's actual stored type didn't
        // match the requested type. Reading as a float first (matching
        // its real JSON type) and rounding into the int field fixes it.
        float maxElF = p["maxEl"] | 0.0f;
        g_issPasses[g_issPassCount].maxElevationDeg = (int)(maxElF + 0.5f);
        g_issPasses[g_issPassCount].magnitude       = p["mag"]      | 99.0f;
        {
          const char* az = p["maxAzCompass"] | "";
          g_issPasses[g_issPassCount].maxAzCompass = String(az);
          g_issPasses[g_issPassCount].maxAz = p["maxAz"] | 0.0f;
        }
        g_issPassCount++;
      }
      if (g_issPassCount > 0) {
        g_iss.nextPassUnix = g_issPasses[0].startUnix;
        g_iss.nextPassDurationSec = (int)(g_issPasses[0].endUnix - g_issPasses[0].startUnix);
      }
      state_unlock();
    }
  } else {
    Serial.printf("[ISS] visualpasses HTTP %d\n", passCode);
  }
  passHttp.end();

  if (!tleLoaded || millis() - lastTleFetchMs > TLE_REFRESH_INTERVAL_MS) {
    if (fetchAndInitTLE()) {
      tleLoaded = true;
      lastTleFetchMs = millis();
    }
  }

  uint32_t crewRefreshInterval = crewCountLoaded ? CREW_REFRESH_INTERVAL_MS : CREW_REFRESH_RETRY_MS;
  if (!crewCountLoaded || millis() - lastCrewFetchMs > crewRefreshInterval) {
    lastCrewFetchMs = millis();
    fetchCrewCount();
  }

  computeGroundTrack();
}
