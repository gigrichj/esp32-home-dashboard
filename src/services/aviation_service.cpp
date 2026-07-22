#include "aviation_service.h"
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <JPEGDEC.h>
#include <esp_heap_caps.h>
#include <WiFiClient.h>
#include <string.h>

Aircraft g_aircraft[MAX_TRACKED_AIRCRAFT];
int g_aircraftCount = 0;
AviationStatus g_aviationStatus;
AircraftDetail g_aircraftDetail;

uint16_t* g_aircraftPhotoPixels = nullptr;
int g_aircraftPhotoWidth = 0;
int g_aircraftPhotoHeight = 0;
bool g_aircraftPhotoValid = false;

static uint16_t* s_decodeTarget = nullptr;
static int s_decodeTargetW = 0;
static int s_decodeTargetH = 0;

static int jpegDrawCallback(JPEGDRAW *pDraw) {
  if (s_decodeTarget == nullptr) return 0;
  for (int row = 0; row < pDraw->iHeight; row++) {
    int destY = pDraw->y + row;
    if (destY < 0 || destY >= s_decodeTargetH) continue;
    uint16_t *destRow = s_decodeTarget + (size_t)destY * s_decodeTargetW;
    const uint16_t *srcRow = pDraw->pPixels + (size_t)row * pDraw->iWidth;
    for (int col = 0; col < pDraw->iWidth; col++) {
      int destX = pDraw->x + col;
      if (destX < 0 || destX >= s_decodeTargetW) continue;
      destRow[destX] = srcRow[col];
    }
  }
  return 1;
}

static void fetchAndDecodePhoto(const String& url) {
  if (url.length() == 0) return;

  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[Aviation] photo fetch HTTP %d\n", code);
    http.end();
    return;
  }

  int len = http.getSize();
  if (len <= 0 || len > 250000) {
    Serial.printf("[Aviation] photo size invalid: %d\n", len);
    http.end();
    return;
  }

  uint8_t *jpegBuf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (jpegBuf == nullptr) {
    Serial.println("[Aviation] photo buffer alloc failed");
    http.end();
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t readTotal = 0;
  uint32_t startMs = millis();
  while (http.connected() && readTotal < (size_t)len && millis() - startMs < 8000) {
    size_t avail = stream->available();
    if (avail > 0) {
      int toRead = (int)min((size_t)avail, (size_t)len - readTotal);
      int r = stream->readBytes(jpegBuf + readTotal, toRead);
      readTotal += r;
    } else {
      delay(1);
    }
  }
  http.end();

  JPEGDEC jpeg;
  if (jpeg.openRAM(jpegBuf, (int)readTotal, jpegDrawCallback)) {
    int w = jpeg.getWidth();
    int h = jpeg.getHeight();

    if (g_aircraftPhotoPixels != nullptr) {
      free(g_aircraftPhotoPixels);
      g_aircraftPhotoPixels = nullptr;
      g_aircraftPhotoValid = false;
    }

    uint16_t *photoBuf = (uint16_t *)heap_caps_malloc((size_t)w * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (photoBuf != nullptr) {
      s_decodeTarget = photoBuf;
      s_decodeTargetW = w;
      s_decodeTargetH = h;
      jpeg.decode(0, 0, 0);
      g_aircraftPhotoPixels = photoBuf;
      g_aircraftPhotoWidth = w;
      g_aircraftPhotoHeight = h;
      g_aircraftPhotoValid = true;
      Serial.printf("[Aviation] photo decoded %dx%d\n", w, h);
    } else {
      Serial.println("[Aviation] photo pixel buffer alloc failed");
    }
    jpeg.close();
  } else {
    Serial.println("[Aviation] JPEG openRAM failed");
  }

  free(jpegBuf);
}

// Both replacements below are volunteer-run, no-auth-required community
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
    Serial.printf("[Aviation] %s JSON parse error: %s\n", sourceName, err.c_str());
    Serial.printf("[Aviation] %s raw payload: %s\n", sourceName, payload.c_str());
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
    Serial.printf("[Aviation] %s HTTP %d\n", sourceName, code);
    g_aviationStatus.lastError = String(sourceName) + " HTTP request failed";
    http.end();
    return false;
  }

  int payloadLen = http.getSize();
  if (payloadLen > 100000) {
    Serial.printf("[Aviation] %s payload implausibly large (%d bytes), skipping\n", sourceName, payloadLen);
    g_aviationStatus.lastError = String(sourceName) + " payload too large, skipped";
    http.end();
    return false;
  }

  int bufSize = (payloadLen > 0) ? payloadLen + 1 : 65536;
  char *rawBuf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
  if (rawBuf == nullptr) {
    Serial.printf("[Aviation] %s payload buffer alloc failed\n", sourceName);
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
  rawBuf[readTotal] = '\0';
  http.end();

  if (readError) {
    Serial.printf("[Aviation] %s payload read error\n", sourceName);
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
}

bool aviation_lookup_flight(const String& flightNumber, Aircraft& out) {
  for (int i = 0; i < g_aircraftCount; i++) {
    if (g_aircraft[i].callsign.equalsIgnoreCase(flightNumber)) {
      out = g_aircraft[i];
      return true;
    }
  }
  return false;
}

void aviation_request_detail(const String& icaoHex, const String& callsign) {
  if (g_aircraftDetail.valid && g_aircraftDetail.lookedUpIcao == icaoHex) {
    return; // cache hit, already have this one
  }
  if (pendingDetailRequested && pendingIcao == icaoHex) {
    return; // already in flight
  }
  pendingIcao = icaoHex;
  pendingCallsign = callsign;
  pendingDetailRequested = true;
  g_aircraftDetail.valid = false;
  g_aircraftDetail.lookupInProgress = true;
  g_aircraftDetail.lookupError = "";
}

static void fetchAircraftType(const String& icaoHex) {
  HTTPClient http;
  String url = "https://api.adsbdb.com/v0/aircraft/" + icaoHex;
  http.begin(url);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      JsonObject aircraft = doc["response"]["aircraft"];
      if (!aircraft.isNull()) {
        g_aircraftDetail.type = aircraft["type"].as<String>();
        const char* thumb = aircraft["url_photo_thumbnail"];
        g_aircraftDetail.photoThumbUrl = thumb ? String(thumb) : String("");
      }
    }
  } else {
    Serial.printf("[Aviation] adsbdb aircraft lookup HTTP %d\n", code);
  }
  http.end();
}

static void fetchRoute(const String& callsign) {
  if (callsign.length() == 0) return;
  HTTPClient http;
  String url = "https://api.adsbdb.com/v0/callsign/" + callsign;
  http.begin(url);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      JsonObject route = doc["response"]["flightroute"];
      if (!route.isNull()) {
        JsonObject origin = route["origin"];
        JsonObject dest = route["destination"];
        if (!origin.isNull()) {
          g_aircraftDetail.originName = origin["municipality"].as<String>();
          g_aircraftDetail.originIata = origin["iata_code"].as<String>();
        }
        if (!dest.isNull()) {
          g_aircraftDetail.destName = dest["municipality"].as<String>();
          g_aircraftDetail.destIata = dest["iata_code"].as<String>();
        }
      }
    }
  } else {
    Serial.printf("[Aviation] adsbdb route lookup HTTP %d\n", code);
  }
  http.end();
}

void aviation_service_detail_loop() {
  if (!pendingDetailRequested) return;
  if (!wifi_manager_is_connected()) return;

  String icaoHex = pendingIcao;
  String callsign = pendingCallsign;
  pendingDetailRequested = false;

  g_aircraftDetail = AircraftDetail();
  g_aircraftDetail.lookupInProgress = true;
  g_aircraftPhotoValid = false;

  fetchAircraftType(icaoHex);
  fetchRoute(callsign);

  if (g_aircraftDetail.photoThumbUrl.length() > 0) {
    fetchAndDecodePhoto(g_aircraftDetail.photoThumbUrl);
  }

  g_aircraftDetail.lookedUpIcao = icaoHex;
  g_aircraftDetail.lookupInProgress = false;
  g_aircraftDetail.valid = true;
}
