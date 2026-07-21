#include "weather_service.h"
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

WeatherData g_weather;
ForecastDay g_forecast[FORECAST_DAYS];
int g_forecastCount = 0;

static void fetchCurrentConditions() {
  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
    "https://api.openweathermap.org/data/2.5/weather?lat=%f&lon=%f&units=imperial&appid=%s",
    (double)HOME_LAT, (double)HOME_LON, OWM_API_KEY);

  http.begin(url);
  int code = http.GET();
  g_weather.lastHttpCode = code;
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      g_weather.tempF       = doc["main"]["temp"]      | 0.0f;
      g_weather.feelsLikeF  = doc["main"]["feels_like"] | 0.0f;
      g_weather.humidity    = doc["main"]["humidity"]   | 0;
      g_weather.windMph     = doc["wind"]["speed"]      | 0.0f;
      g_weather.windGustMph = doc["wind"]["gust"]        | g_weather.windMph;
      g_weather.windDeg     = doc["wind"]["deg"]         | 0.0f;
      g_weather.condition   = doc["weather"][0]["main"].as<String>();
      g_weather.weatherId   = doc["weather"][0]["id"]   | 0;
      g_weather.sunriseUnix = doc["sys"]["sunrise"]      | 0;
      g_weather.sunsetUnix  = doc["sys"]["sunset"]       | 0;

      // Dew point via the Magnus formula -- not present in the free
      // current-conditions endpoint, but derivable from temp + humidity,
      // which we already have. Accurate to within about +/-0.4C.
      {
        float tempC = (g_weather.tempF - 32.0f) * 5.0f / 9.0f;
        float rh = (float)g_weather.humidity;
        if (rh < 1.0f) rh = 1.0f; // avoid log(0)
        float a = 17.27f, b = 237.7f;
        float alpha = logf(rh / 100.0f) + (a * tempC) / (b + tempC);
        float dewC = (b * alpha) / (a - alpha);
        g_weather.dewPointF = dewC * 9.0f / 5.0f + 32.0f;
      }

      g_weather.valid = true;
    } else {
      Serial.printf("[Weather] JSON parse error: %s\n", err.c_str());
      Serial.printf("[Weather] raw payload: %s\n", payload.c_str());
    }
  } else {
    Serial.printf("[Weather] HTTP %d\n", code);
  }
  http.end();
}

static void fetchForecast() {
  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
    "https://api.openweathermap.org/data/2.5/forecast?lat=%f&lon=%f&units=imperial&appid=%s",
    (double)HOME_LAT, (double)HOME_LON, OWM_API_KEY);

  http.begin(url);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[Weather] Forecast HTTP %d\n", code);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[Weather] Forecast JSON parse error: %s\n", err.c_str());
    return;
  }

  long tzOffsetSec = doc["city"]["timezone"] | 0;
  if (g_weather.sunriseUnix == 0) g_weather.sunriseUnix = doc["city"]["sunrise"] | 0;
  if (g_weather.sunsetUnix == 0)  g_weather.sunsetUnix  = doc["city"]["sunset"]  | 0;

  JsonArray list = doc["list"].as<JsonArray>();

  // Precipitation chance isn't in the current-conditions call; pull it from
  // the nearest upcoming forecast entry instead.
  if (list.size() > 0) {
    float pop = list[0]["pop"] | 0.0f;
    g_weather.precipChance = (int)(pop * 100.0f + 0.5f);
  }

  struct DayAccum {
    int dayOfYear = -1;
    float highF = -999.0f;
    float lowF = 999.0f;
    int weatherId = 0;
    int bestHourDist = 999;
    char label[4] = "";
  };
  static const int MAX_DAYS = 6;
  DayAccum days[MAX_DAYS];
  int dayCount = 0;
  static const char* DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

  for (JsonObject entry : list) {
    time_t rawDt = entry["dt"] | 0;
    time_t shifted = rawDt + tzOffsetSec;
    struct tm tmInfo;
    gmtime_r(&shifted, &tmInfo);

    int doy = tmInfo.tm_yday;
    int idx = -1;
    for (int i = 0; i < dayCount; i++) {
      if (days[i].dayOfYear == doy) { idx = i; break; }
    }
    if (idx == -1) {
      if (dayCount >= MAX_DAYS) continue;
      idx = dayCount++;
      days[idx].dayOfYear = doy;
      snprintf(days[idx].label, sizeof(days[idx].label), "%s", DOW[tmInfo.tm_wday]);
    }

    float tempF = entry["main"]["temp"] | 0.0f;
    if (tempF > days[idx].highF) days[idx].highF = tempF;
    if (tempF < days[idx].lowF)  days[idx].lowF  = tempF;

    int hourDist = abs(tmInfo.tm_hour - 12);
    if (hourDist < days[idx].bestHourDist) {
      days[idx].bestHourDist = hourDist;
      days[idx].weatherId = entry["weather"][0]["id"] | 0;
    }
  }

  // Skip today (usually a partial day) if we have enough full days to
  // fill a 5-day strip; otherwise show what's available.
  int startIdx = (dayCount > FORECAST_DAYS) ? 1 : 0;
  g_forecastCount = 0;
  for (int i = startIdx; i < dayCount && g_forecastCount < FORECAST_DAYS; i++) {
    g_forecast[g_forecastCount].dayLabel  = String(days[i].label);
    g_forecast[g_forecastCount].highF     = days[i].highF;
    g_forecast[g_forecastCount].lowF      = days[i].lowF;
    g_forecast[g_forecastCount].weatherId = days[i].weatherId;
    g_forecastCount++;
  }
}

void weather_service_update() {
  if (!wifi_manager_is_connected()) return;
  fetchCurrentConditions();
  fetchForecast();
}
