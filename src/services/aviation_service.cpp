#include "aviation_service.h"
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

Aircraft g_aircraft[MAX_TRACKED_AIRCRAFT];
int g_aircraftCount = 0;
AviationStatus g_aviationStatus;
AircraftDetail g_aircraftDetail;

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

  fetchAircraftType(icaoHex);
  fetchRoute(callsign);

  g_aircraftDetail.lookedUpIcao = icaoHex;
  g_aircraftDetail.lookupInProgress = false;
  g_aircraftDetail.valid = true;
}
