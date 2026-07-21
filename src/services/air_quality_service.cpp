#include "air_quality_service.h"
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>

AirQualityData g_airQuality;

const char* air_quality_label(int aqi) {
  switch (aqi) {
    case 1: return "Good";
    case 2: return "Fair";
    case 3: return "Moderate";
    case 4: return "Poor";
    case 5: return "Very Poor";
    default: return "--";
  }
}

void air_quality_service_update() {
  if (!wifi_manager_is_connected()) return;

  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
    "https://api.openweathermap.org/data/2.5/air_pollution?lat=%f&lon=%f&appid=%s",
    (double)HOME_LAT, (double)HOME_LON, OWM_API_KEY);

  http.begin(url);
  int code = http.GET();
  g_airQuality.lastHttpCode = code;
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      JsonObject item = doc["list"][0];
      g_airQuality.aqi   = item["main"]["aqi"] | 0;
      g_airQuality.pm2_5 = item["components"]["pm2_5"] | 0.0f;
      g_airQuality.pm10  = item["components"]["pm10"]  | 0.0f;
      g_airQuality.valid = true;
    } else {
      Serial.printf("[AirQuality] JSON parse error: %s\n", err.c_str());
    }
  } else {
    Serial.printf("[AirQuality] HTTP %d\n", code);
  }
  http.end();
}
