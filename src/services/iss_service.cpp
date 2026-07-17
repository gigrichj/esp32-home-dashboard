#include "iss_service.h"
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>

IssData g_iss;

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
    JsonDocument doc;
    if (!deserializeJson(doc, http.getStream())) {
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
    "https://api.n2yo.com/rest/v1/satellite/visualpasses/25544/%f/%f/0/2/300/&apiKey=%s",
    (double)HOME_LAT, (double)HOME_LON, N2YO_API_KEY);

  passHttp.begin(passUrl);
  int passCode = passHttp.GET();
  if (passCode == 200) {
    JsonDocument passDoc;
    if (!deserializeJson(passDoc, passHttp.getStream())) {
      JsonArray passes = passDoc["passes"].as<JsonArray>();
      if (passes.size() > 0) {
        JsonObject firstPass = passes[0];
        g_iss.nextPassUnix = firstPass["startUTC"] | 0;
        g_iss.nextPassDurationSec = firstPass["duration"] | 0;
      }
    }
  } else {
    Serial.printf("[ISS] visualpasses HTTP %d\n", passCode);
  }
  passHttp.end();
}
