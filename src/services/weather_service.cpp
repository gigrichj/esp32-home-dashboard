#include "weather_service.h"
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>

WeatherData g_weather;

void weather_service_update() {
  if (!wifi_manager_is_connected()) return;

  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
    "https://api.openweathermap.org/data/2.5/weather?lat=%f&lon=%f&units=imperial&appid=%s",
    (double)HOME_LAT, (double)HOME_LON, OWM_API_KEY);

  http.begin(url);
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc; // ArduinoJson v7 auto-sizes
    DeserializationError err = deserializeJson(doc, http.getStream());
    if (!err) {
      g_weather.tempF      = doc["main"]["temp"]       | 0.0f;
      g_weather.feelsLikeF = doc["main"]["feels_like"]  | 0.0f;
      g_weather.humidity   = doc["main"]["humidity"]    | 0;
      g_weather.windMph    = doc["wind"]["speed"]       | 0.0f;
      g_weather.condition  = doc["weather"][0]["main"].as<String>();
      g_weather.valid = true;
    } else {
      Serial.printf("[Weather] JSON parse error: %s\n", err.c_str());
    }
  } else {
    Serial.printf("[Weather] HTTP %d\n", code);
  }
  http.end();

  // TODO: second call to /onecall or /forecast for hourly + 7-day,
  // and a static radar image fetch (see aviation_service for the
  // same "fetch raw JPEG bytes" pattern you'd reuse here).
}
