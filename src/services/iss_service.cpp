#include "iss_service.h"
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Sgp4.h>
#include <time.h>

IssData g_iss;
IssPass g_issPasses[ISS_MAX_PASSES];
int g_issPassCount = 0;
int g_issCrewCount = 0;
int g_issCrewLastHttpCode = -999;
TrackPoint g_issTrack[ISS_TRACK_POINTS];
int g_issTrackCount = 0;
bool g_issTrackValid = false;

static Sgp4 sat;
static bool tleLoaded = false;
static uint32_t lastTleFetchMs = 0;
static const uint32_t TLE_REFRESH_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL; // 6 hours
static const int TRACK_STEP_SECONDS = 100; // 60 pts * 100s = 6000s (~100min), > one ISS orbit (~92min)

// Fetches the current TLE from CelesTrak (free, no key needed) and loads it
// into the SGP4 propagator. TLEs don't change fast, so this only needs to
// run every few hours; the ground-track math itself is pure local
// computation with zero network cost once a TLE is loaded.
static void fetchCrewCount() {
  HTTPClient http;
  http.begin("https://api.open-notify.org/astros.json");
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); // same class of fix
                          // as the 7Timer 302 issue -- Open Notify may
                          // also redirect, and HTTPClient doesn't follow
                          // redirects by default.
  http.addHeader("User-Agent", "ESP32-Home-Dashboard/1.0");
  int code = http.GET();
  g_issCrewLastHttpCode = code;
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      JsonArray people = doc["people"].as<JsonArray>();
      int count = 0;
      for (JsonObject p : people) {
        const char* craft = p["craft"] | "";
        if (String(craft) == "ISS") count++;
      }
      g_issCrewCount = count;
    }
  } else {
    Serial.printf("[ISS] astros.json HTTP %d\n", code);
  }
  http.end();
}

static bool fetchAndInitTLE() {
  HTTPClient http;
  http.begin("https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[ISS] TLE fetch HTTP %d\n", code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  int nl1 = payload.indexOf('\n');
  int nl2 = payload.indexOf('\n', nl1 + 1);
  if (nl1 < 0 || nl2 < 0) {
    Serial.println("[ISS] TLE response missing expected line breaks");
    return false;
  }

  String nameLine = payload.substring(0, nl1);
  String line1 = payload.substring(nl1 + 1, nl2);
  String line2 = payload.substring(nl2 + 1);
  nameLine.trim();
  line1.trim();
  line2.trim();

  if (line1.length() < 60 || line2.length() < 60) {
    Serial.println("[ISS] TLE lines look truncated, skipping");
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
  return true;
}

static void computeGroundTrack() {
  if (!tleLoaded) {
    g_issTrackValid = false;
    return;
  }

  uint32_t nowUnix = (uint32_t)time(nullptr);
  if (nowUnix < 100000) {
    g_issTrackValid = false; // clock not synced yet
    return;
  }

  g_issTrackCount = 0;
  for (int i = 0; i < ISS_TRACK_POINTS; i++) {
    unsigned long t = (unsigned long)nowUnix + (unsigned long)(i * TRACK_STEP_SECONDS);
    sat.findsat(t);
    g_issTrack[g_issTrackCount].lat = sat.satLat;
    g_issTrack[g_issTrackCount].lon = sat.satLon;
    g_issTrackCount++;
  }
  g_issTrackValid = true;
}

void iss_service_update() {
  if (!wifi_manager_is_connected()) return;

  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
    "https://api.n2yo.com/rest/v1/satellite/positions/25544/%f/%f/0/1/&apiKey=%s",
    (double)HOME_LAT, (double)HOME_LON, N2YO_API_KEY);

  http.begin(url);
  int code = http.GET();
  g_iss.lastHttpCode = code;
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      Serial.printf("[ISS] JSON parse error: %s\n", err.c_str());
      Serial.printf("[ISS] raw payload: %s\n", payload.c_str());
    } else {
      JsonObject pos = doc["positions"][0];
      g_iss.lat        = pos["satlatitude"]  | 0.0f;
      g_iss.lon        = pos["satlongitude"] | 0.0f;
      g_iss.altitudeKm = pos["sataltitude"]  | 0.0f;
      g_iss.valid = true;
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
  int passCode = passHttp.GET();
  if (passCode == 200) {
    String passPayload = passHttp.getString();
    JsonDocument passDoc;
    if (!deserializeJson(passDoc, passPayload)) {
      JsonArray passes = passDoc["passes"].as<JsonArray>();
      g_issPassCount = 0;
      for (JsonObject p : passes) {
        if (g_issPassCount >= ISS_MAX_PASSES) break;
        g_issPasses[g_issPassCount].startUnix       = p["startUTC"] | 0;
        g_issPasses[g_issPassCount].endUnix         = p["endUTC"]   | 0;
        g_issPasses[g_issPassCount].maxElevationDeg = p["maxEl"]    | 0;
        g_issPasses[g_issPassCount].magnitude       = p["mag"]      | 99.0f;
        {
          const char* az = p["maxAzCompass"] | "";
          g_issPasses[g_issPassCount].maxAzCompass = String(az);
        }
        g_issPassCount++;
      }
      if (g_issPassCount > 0) {
        g_iss.nextPassUnix = g_issPasses[0].startUnix;
        g_iss.nextPassDurationSec = (int)(g_issPasses[0].endUnix - g_issPasses[0].startUnix);
      }
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
    fetchCrewCount();
  }
  computeGroundTrack();
}
