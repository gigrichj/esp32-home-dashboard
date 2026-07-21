#include "aviation_service.h"
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <JPEGDEC.h>
#include <esp_heap_caps.h>
#include <WiFiClient.h>

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

static const char* OPENSKY_TOKEN_URL =
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
    Serial.printf("[Aviation] Token request HTTP %d\n", code);
    g_aviationStatus.lastError = "OAuth token request failed";
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[Aviation] Token JSON parse error: %s\n", err.c_str());
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
      Serial.printf("[Aviation] states payload implausibly large (%d bytes), skipping\n", payloadLen);
      g_aviationStatus.lastError = "States payload too large, skipped";
      http.end();
      return;
    }
    String payload = http.getString();
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
      Serial.printf("[Aviation] JSON parse error: %s\n", err.c_str());
      Serial.printf("[Aviation] raw payload: %s\n", payload.c_str());
      g_aviationStatus.lastError = String("JSON parse: ") + err.c_str();
    }
  } else if (code == 401) {
    Serial.println("[Aviation] HTTP 401 - token expired/invalid, will refresh next poll");
    tokenExpiryMillis = 0;
    g_aviationStatus.lastError = "Token expired (401), will retry";
  } else {
    Serial.printf("[Aviation] HTTP %d\n", code);
    g_aviationStatus.lastError = "HTTP request failed";
  }
  http.end();
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
