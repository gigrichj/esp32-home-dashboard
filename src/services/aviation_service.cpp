#include "aviation_service.h"
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

Aircraft g_aircraft[MAX_TRACKED_AIRCRAFT];
int g_aircraftCount = 0;

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

void aviation_service_update() {
  if (!wifi_manager_is_connected()) return;

  // ADS-B Exchange RapidAPI-style bounding-box endpoint (adjust host/path
  // to whichever ADSBX tier you're on — the free lol/rapidapi vs.
  // self-hosted feed have slightly different URLs).
  HTTPClient http;
  char url[256];
  float radiusDeg = 0.7f; // roughly ~40nm box around home
  snprintf(url, sizeof(url),
    "https://adsbexchange-com1.p.rapidapi.com/v2/lat/%f/lon/%f/dist/40/",
    (double)HOME_LAT, (double)HOME_LON);

  http.begin(url);
  http.addHeader("X-RapidAPI-Key", ADSBX_API_KEY);
  http.addHeader("X-RapidAPI-Host", "adsbexchange-com1.p.rapidapi.com");
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      JsonArray ac = doc["ac"].as<JsonArray>();
      g_aircraftCount = 0;
      for (JsonObject a : ac) {
        if (g_aircraftCount >= MAX_TRACKED_AIRCRAFT) break;
        Aircraft &out = g_aircraft[g_aircraftCount];
        out.icao          = a["hex"].as<String>();
        out.callsign      = a["flight"].as<String>();
        out.lat           = a["lat"] | 0.0f;
        out.lon           = a["lon"] | 0.0f;
        out.altitudeFt    = a["alt_baro"] | 0;
        out.groundSpeedKt = a["gs"] | 0;
        out.trackDeg      = a["track"] | 0.0f;
        out.bearingFromHome = bearingDeg(HOME_LAT, HOME_LON, out.lat, out.lon);
        out.distanceNm      = distanceNm(HOME_LAT, HOME_LON, out.lat, out.lon);
        g_aircraftCount++;
      }
    } else {
      Serial.printf("[Aviation] JSON parse error: %s\n", err.c_str());
      Serial.printf("[Aviation] raw payload: %s\n", payload.c_str());
    }
  } else {
    Serial.printf("[Aviation] HTTP %d\n", code);
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
  return false; // TODO: fall back to a direct FlightAware AeroAPI call if not in local list
}
