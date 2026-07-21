#!/usr/bin/env bash
set -e

python3 << 'PYEOF'
path = "src/services/aviation_service.cpp"
with open(path) as f:
    content = f.read()

old = '''static const char* OPENSKY_TOKEN_URL =
  "https://auth.opensky-network.org/auth/realms/opensky-network/protocol/openid-connect/token";
static const char* OPENSKY_STATES_URL = "https://opensky-network.org/api/states/all";

static String oauthToken;
static uint32_t tokenExpiryMillis = 0;

static bool pendingDetailRequested = false;
static String pendingIcao;
static String pendingCallsign;

static float bearingDeg(float lat1, float lon1, float lat2, float lon2) {
  float dLon = radians(lon2 - lon1);
  lat1 = radians(lat1); lat2 = radians(lat2);
  float y = sin(dLon) * cos(lat2);
  float x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dLon);
  float brng = degrees(atan2(y, x));
  return fmod(brng + 360.0f, 360.0f);
}

static float distanceNm(float lat1, float lon1, float lat2, float lon2) {
  const float R_NM = 3440.065f;
  float dLat = radians(lat2 - lat1);
  float dLon = radians(lon2 - lon1);
  float a = sin(dLat/2) * sin(dLat/2) +
            cos(radians(lat1)) * cos(radians(lat2)) * sin(dLon/2) * sin(dLon/2);
  return R_NM * 2 * atan2(sqrt(a), sqrt(1 - a));
}

static bool ensureOpenSkyToken() {
  if (oauthToken.length() > 0 && millis() < tokenExpiryMillis) {
    return true;
  }

  HTTPClient http;
  http.begin(OPENSKY_TOKEN_URL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = String("grant_type=client_credentials&client_id=") + OPENSKY_CLIENT_ID +
                "&client_secret=" + OPENSKY_CLIENT_SECRET;

  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("[Aviation] Token request HTTP %d\\n", code);
    g_aviationStatus.lastError = "OAuth token request failed";
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[Aviation] Token JSON parse error: %s\\n", err.c_str());
    g_aviationStatus.lastError = "OAuth token parse error";
    return false;
  }

  const char* token = doc["access_token"];
  int expiresIn = doc["expires_in"] | 1800;
  if (!token) {
    g_aviationStatus.lastError = "OAuth response missing access_token";
    return false;
  }

  oauthToken = String(token);
  tokenExpiryMillis = millis() + (uint32_t)(expiresIn - 60) * 1000UL;
  Serial.println("[Aviation] OAuth token refreshed");
  return true;
}

void aviation_service_update() {
  if (!wifi_manager_is_connected()) return;

  if (!ensureOpenSkyToken()) {
    g_aviationStatus.lastHttpCode = -1;
    return;
  }

  float latSpan = 40.0f / 60.0f;
  float lonSpan = latSpan / cos(radians((double)HOME_LAT));

  char url[256];
  snprintf(url, sizeof(url),
    "%s?lamin=%f&lomin=%f&lamax=%f&lomax=%f",
    OPENSKY_STATES_URL,
    (double)HOME_LAT - latSpan, (double)HOME_LON - lonSpan,
    (double)HOME_LAT + latSpan, (double)HOME_LON + lonSpan);

  HTTPClient http;
  http.begin(url);
  http.addHeader("Authorization", "Bearer " + oauthToken);
  int code = http.GET();
  g_aviationStatus.lastHttpCode = code;

  if (code == 200) {
    int payloadLen = http.getSize();
    // Guard against an implausibly large response -- the lamin/lomin/lamax/lomax
    // bounding box should keep this to a small local dataset. If OpenSky ever
    // ignores the bounding box (rate limiting, auth hiccup, API change) and
    // returns the full global states dataset instead, it can be several MB;
    // letting http.getString() try to allocate that is suspected of causing
    // the standalone heap-exhaustion crashes seen with an empty aircraft list.
    if (payloadLen > 100000) {
      Serial.printf("[Aviation] states payload implausibly large (%d bytes), skipping\\n", payloadLen);
      g_aviationStatus.lastError = "States payload too large, skipped";
      http.end();
      return;
    }

    // Read the body manually with an explicit timeout and a yield on every
    // iteration, instead of http.getString() -- that call (via
    // Stream::readString()) reads one byte at a time in a tight loop with no
    // yield, and if the peer resets the connection mid-read (observed:
    // errno 104 "Connection reset by peer"), the underlying mbedtls_ssl_read
    // can block long enough to starve the CPU 0 idle task and trip the
    // FreeRTOS task watchdog, crashing the whole device. This mirrors the
    // safe streaming pattern already used in fetchAndDecodePhoto() above.
    int bufSize = (payloadLen > 0) ? payloadLen + 1 : 65536;
    char *rawBuf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
    if (rawBuf == nullptr) {
      Serial.println("[Aviation] states payload buffer alloc failed");
      g_aviationStatus.lastError = "Buffer alloc failed";
      http.end();
      return;
    }

    WiFiClient *stream = http.getStreamPtr();
    size_t readTotal = 0;
    uint32_t startMs = millis();
    bool readError = false;
    while (readTotal < (size_t)(bufSize - 1) && millis() - startMs < 8000) {
      if (!http.connected() && stream->available() == 0) break;
      size_t avail = stream->available();
      if (avail > 0) {
        int toRead = (int)min(avail, (size_t)(bufSize - 1 - readTotal));
        int r = stream->readBytes(rawBuf + readTotal, toRead);
        if (r <= 0) { readError = true; break; }
        readTotal += r;
      } else {
        vTaskDelay(pdMS_TO_TICKS(5)); // yield -- this is the critical fix
      }
    }
    rawBuf[readTotal] = '\\0';

    if (readError) {
      Serial.println("[Aviation] states payload read error");
      g_aviationStatus.lastError = "States payload read error";
      free(rawBuf);
      http.end();
      return;
    }

    String payload(rawBuf);
    free(rawBuf);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      g_aviationStatus.lastError = "";
      JsonArray states = doc["states"].as<JsonArray>();
      g_aircraftCount = 0;
      for (JsonArray s : states) {
        if (g_aircraftCount >= MAX_TRACKED_AIRCRAFT) break;
        if (s.size() < 11) continue;
        if (s[5].isNull() || s[6].isNull()) continue;
        bool onGround = s[8] | false;
        if (onGround) continue;

        Aircraft &out = g_aircraft[g_aircraftCount];
        out.icao = s[0].as<String>();
        String callsign = s[1].as<String>();
        callsign.trim();
        out.callsign = callsign;
        out.lon = s[5] | 0.0f;
        out.lat = s[6] | 0.0f;
        float baroAltM = s[7] | 0.0f;
        out.altitudeFt = (int)(baroAltM * 3.28084f);
        float velocityMs = s[9] | 0.0f;
        out.groundSpeedKt = (int)(velocityMs * 1.94384f);
        out.trackDeg = s[10] | 0.0f;

        // vertical_rate (index 11, m/s) and squawk (index 14) - fields
        // OpenSky already gives us, just not parsed until now.
        if (s.size() > 11 && !s[11].isNull()) {
          float vRateMs = s[11] | 0.0f;
          out.verticalRateFpm = (int)(vRateMs * 196.850f);
        } else {
          out.verticalRateFpm = 0;
        }
        if (s.size() > 14 && !s[14].isNull()) {
          out.squawk = s[14].as<String>();
        } else {
          out.squawk = "";
        }

        out.bearingFromHome = bearingDeg(HOME_LAT, HOME_LON, out.lat, out.lon);
        out.distanceNm = distanceNm(HOME_LAT, HOME_LON, out.lat, out.lon);
        g_aircraftCount++;
      }
    } else {
      Serial.printf("[Aviation] JSON parse error: %s\\n", err.c_str());
      Serial.printf("[Aviation] raw payload: %s\\n", payload.c_str());
      g_aviationStatus.lastError = String("JSON parse: ") + err.c_str();
    }
  } else if (code == 401) {
    Serial.println("[Aviation] HTTP 401 - token expired/invalid, will refresh next poll");
    tokenExpiryMillis = 0;
    g_aviationStatus.lastError = "Token expired (401), will retry";
  } else {
    Serial.printf("[Aviation] HTTP %d\\n", code);
    g_aviationStatus.lastError = "HTTP request failed";
  }
  http.end();
}'''

assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"

new = '''// Both replacements below are volunteer-run, no-auth-required community
// ADS-B feeds (adsb.fi and airplanes.live), swapped in after OpenSky's
// OAuth-gated states endpoint proved unreliable in practice: frequent
// "Connection reset by peer" errors and strict rate limiting on the free
// tier. Both use the same ADSBExchange v2-compatible response format (a
// top-level "ac" array of named-field objects), so one parser covers both,
// and dropping OAuth entirely removes a whole class of token-refresh
// failure modes. adsb.fi is tried first; airplanes.live is an automatic
// fallback on the same poll cycle if adsb.fi fails for any reason.
static const int AVIATION_RANGE_NM = 40;

static bool pendingDetailRequested = false;
static String pendingIcao;
static String pendingCallsign;

static float bearingDeg(float lat1, float lon1, float lat2, float lon2) {
  float dLon = radians(lon2 - lon1);
  lat1 = radians(lat1); lat2 = radians(lat2);
  float y = sin(dLon) * cos(lat2);
  float x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dLon);
  float brng = degrees(atan2(y, x));
  return fmod(brng + 360.0f, 360.0f);
}

static float distanceNm(float lat1, float lon1, float lat2, float lon2) {
  const float R_NM = 3440.065f;
  float dLat = radians(lat2 - lat1);
  float dLon = radians(lon2 - lon1);
  float a = sin(dLat/2) * sin(dLat/2) +
            cos(radians(lat1)) * cos(radians(lat2)) * sin(dLon/2) * sin(dLon/2);
  return R_NM * 2 * atan2(sqrt(a), sqrt(1 - a));
}

// Parses an ADSBExchange v2-compatible "ac" array (shared format between
// adsb.fi and airplanes.live) into g_aircraft/g_aircraftCount.
static bool parseAircraftJson(const String& payload, const char* sourceName) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[Aviation] %s JSON parse error: %s\\n", sourceName, err.c_str());
    Serial.printf("[Aviation] %s raw payload: %s\\n", sourceName, payload.c_str());
    g_aviationStatus.lastError = String(sourceName) + " JSON parse: " + err.c_str();
    return false;
  }

  JsonArray ac = doc["ac"].as<JsonArray>();
  g_aircraftCount = 0;
  for (JsonObject a : ac) {
    if (g_aircraftCount >= MAX_TRACKED_AIRCRAFT) break;

    // alt_baro is either a number (feet) or the literal string "ground" --
    // skip aircraft on the ground, and skip anything with no altitude at all.
    JsonVariant altVar = a["alt_baro"];
    if (altVar.isNull()) continue;
    if (altVar.is<const char*>()) {
      const char* altStr = altVar.as<const char*>();
      if (altStr && strcmp(altStr, "ground") == 0) continue;
    }

    const char* hex = a["hex"] | "";
    if (strlen(hex) == 0) continue;

    float lat = a["lat"] | 0.0f;
    float lon = a["lon"] | 0.0f;
    if (lat == 0.0f && lon == 0.0f) continue; // no position, skip

    Aircraft &out = g_aircraft[g_aircraftCount];
    out.icao = String(hex);
    String callsign = a["flight"] | "";
    callsign.trim();
    out.callsign = callsign;
    out.lat = lat;
    out.lon = lon;
    out.altitudeFt = altVar.is<int>() ? altVar.as<int>() : (int)(altVar.as<float>());
    out.groundSpeedKt = (int)(a["gs"] | 0.0f);
    out.trackDeg = a["track"] | 0.0f;
    out.verticalRateFpm = (int)(a["baro_rate"] | 0.0f);
    out.squawk = a["squawk"] | "";

    out.bearingFromHome = bearingDeg(HOME_LAT, HOME_LON, out.lat, out.lon);
    out.distanceNm = distanceNm(HOME_LAT, HOME_LON, out.lat, out.lon);
    g_aircraftCount++;
  }

  g_aviationStatus.lastError = "";
  return true;
}

// Fetches from a single source URL using the same yield-safe manual read
// loop established in fetchAndDecodePhoto()/the old OpenSky path -- reads
// one chunk at a time with a 5ms yield when nothing is available yet,
// instead of http.getString()'s no-yield tight loop, so a reset/stalled
// connection can never block long enough to trip the task watchdog.
static bool fetchAircraftFromUrl(const char* url, const char* sourceName) {
  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  g_aviationStatus.lastHttpCode = code;

  if (code != 200) {
    Serial.printf("[Aviation] %s HTTP %d\\n", sourceName, code);
    g_aviationStatus.lastError = String(sourceName) + " HTTP request failed";
    http.end();
    return false;
  }

  int payloadLen = http.getSize();
  if (payloadLen > 100000) {
    Serial.printf("[Aviation] %s payload implausibly large (%d bytes), skipping\\n", sourceName, payloadLen);
    g_aviationStatus.lastError = String(sourceName) + " payload too large, skipped";
    http.end();
    return false;
  }

  int bufSize = (payloadLen > 0) ? payloadLen + 1 : 65536;
  char *rawBuf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
  if (rawBuf == nullptr) {
    Serial.printf("[Aviation] %s payload buffer alloc failed\\n", sourceName);
    g_aviationStatus.lastError = "Buffer alloc failed";
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t readTotal = 0;
  uint32_t startMs = millis();
  bool readError = false;
  while (readTotal < (size_t)(bufSize - 1) && millis() - startMs < 8000) {
    if (!http.connected() && stream->available() == 0) break;
    size_t avail = stream->available();
    if (avail > 0) {
      int toRead = (int)min(avail, (size_t)(bufSize - 1 - readTotal));
      int r = stream->readBytes(rawBuf + readTotal, toRead);
      if (r <= 0) { readError = true; break; }
      readTotal += r;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5)); // yield -- the critical fix from v128
    }
  }
  rawBuf[readTotal] = '\\0';
  http.end();

  if (readError) {
    Serial.printf("[Aviation] %s payload read error\\n", sourceName);
    g_aviationStatus.lastError = String(sourceName) + " payload read error";
    free(rawBuf);
    return false;
  }

  String payload(rawBuf);
  free(rawBuf);

  return parseAircraftJson(payload, sourceName);
}

void aviation_service_update() {
  if (!wifi_manager_is_connected()) return;

  char adsbFiUrl[160];
  snprintf(adsbFiUrl, sizeof(adsbFiUrl),
    "https://opendata.adsb.fi/api/v3/lat/%f/lon/%f/dist/%d",
    (double)HOME_LAT, (double)HOME_LON, AVIATION_RANGE_NM);

  if (fetchAircraftFromUrl(adsbFiUrl, "adsb.fi")) {
    return;
  }

  Serial.println("[Aviation] adsb.fi failed, falling back to airplanes.live");

  char airplanesLiveUrl[160];
  snprintf(airplanesLiveUrl, sizeof(airplanesLiveUrl),
    "https://api.airplanes.live/v2/point/%f/%f/%d",
    (double)HOME_LAT, (double)HOME_LON, AVIATION_RANGE_NM);

  fetchAircraftFromUrl(airplanesLiveUrl, "airplanes.live");
}'''

content = content.replace(old, new)

# Ensure strlen/strcmp are available
if "#include <string.h>" not in content:
    content = content.replace('#include <WiFiClient.h>', '#include <WiFiClient.h>\n#include <string.h>', 1)

with open(path, "w") as f:
    f.write(content)
print("Replaced OpenSky OAuth flow with adsb.fi (primary) + airplanes.live (fallback), no auth needed")

import re
vpath = "src/version.h"
with open(vpath) as f:
    vcontent = f.read()
m = re.search(r'#define FIRMWARE_VERSION "v(\d+)"', vcontent)
assert m, "could not find FIRMWARE_VERSION define"
old_ver_line = m.group(0)
new_ver_num = int(m.group(1)) + 1
new_ver_line = f'#define FIRMWARE_VERSION "v{new_ver_num}"'
assert vcontent.count(old_ver_line) == 1
vcontent = vcontent.replace(old_ver_line, new_ver_line)
with open(vpath, "w") as f:
    f.write(vcontent)
print(f"Bumped version.h: {old_ver_line} -> {new_ver_line}")
PYEOF
