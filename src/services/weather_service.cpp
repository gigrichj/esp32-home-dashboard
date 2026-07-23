#include "weather_service.h"
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <time.h>
#include <math.h>

WeatherData g_weather;
ForecastDay g_forecast[FORECAST_DAYS];
int g_forecastCount = 0;

HourlyPrecipPoint g_precipHourly[PRECIP_HOURLY_POINTS];
int g_precipHourlyCount = 0;
bool g_precipHourlyValid = false;
int g_precipHourlyLastHttpCode = -999;

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

      // TEMP DEBUG (v113): font-safe on-screen trace (serial monitor is
      // unavailable), since our bitmap font can't render braces or quotes.
      // Remove once root cause of sunrise/sunset being 0 is confirmed.
      {
        bool hasSys = doc["sys"].is<JsonObject>();
        char dbg[64];
        snprintf(dbg, sizeof(dbg), "HAS SYS %s SR %lu SS %lu",
                 hasSys ? "Y" : "N",
                 (unsigned long)g_weather.sunriseUnix,
                 (unsigned long)g_weather.sunsetUnix);
        g_weather.debugSunLine = String(dbg);
      }

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

// UV index isn't on OpenWeatherMap's free tier, so it's fetched from
// Open-Meteo instead (already used as the Astro page's 7Timer fallback
// source). Pulls today's hourly uv_index array and picks whichever hour
// is closest to right now, which is simpler and just as accurate as
// trying to interpolate between two hours for a slowly-changing value.
static void fetchUvIndex() {
  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast?latitude=%f&longitude=%f&hourly=uv_index&forecast_days=1&timezone=UTC",
    (double)HOME_LAT, (double)HOME_LON);

  http.begin(url);
  // Open-Meteo has been observed serving chunked transfer encoding on
  // larger JSON responses (see astro_seeing_service.cpp's v141/v148 notes)
  // -- forcing HTTP/1.0 gets a plain Content-Length body instead, which is
  // what plain http.getString() below expects.
  http.useHTTP10(true);
  int code = http.GET();
  g_weather.uvLastHttpCode = code;
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      JsonArray times = doc["hourly"]["time"].as<JsonArray>();
      JsonArray uvValues = doc["hourly"]["uv_index"].as<JsonArray>();
      time_t now = time(nullptr);
      int bestIdx = -1;
      long bestDist = LONG_MAX;
      int i = 0;
      for (JsonVariant tEntry : times) {
        // Open-Meteo times come back as "YYYY-MM-DDTHH:MM" in UTC (matches
        // the timezone=UTC param above) -- parse just the hour-of-day part
        // and compare against the current UTC hour, which is enough
        // resolution for a value that only changes hour-to-hour anyway.
        String tStr = tEntry.as<String>();
        if (tStr.length() >= 13) {
          int entryHour = tStr.substring(11, 13).toInt();
          struct tm* utcNow = gmtime(&now);
          long dist = labs((long)entryHour - (long)utcNow->tm_hour);
          if (dist < bestDist) {
            bestDist = dist;
            bestIdx = i;
          }
        }
        i++;
      }
      if (bestIdx >= 0 && bestIdx < (int)uvValues.size()) {
        g_weather.uvIndex = uvValues[bestIdx] | 0.0f;
        g_weather.uvValid = true;
      }
    } else {
      Serial.printf("[Weather] UV JSON parse error: %s\n", err.c_str());
    }
  } else {
    Serial.printf("[Weather] UV HTTP %d\n", code);
  }
  http.end();
}

// Same yield-safe manual read-loop as astro_seeing_service.cpp's
// readHttpBodySafely() -- duplicated locally as a small per-file static
// helper (same pattern already used for utcTmToUnix below) rather than
// shared across services, since a plain http.getString() here risks the
// same FreeRTOS watchdog crash already fixed in aviation/air-quality.
static bool readHttpBodySafely(HTTPClient& http, String& outPayload, const char* sourceName) {
  int payloadLen = http.getSize();
  int bufSize = (payloadLen > 0) ? payloadLen + 1 : 32768;
  char *rawBuf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
  if (rawBuf == nullptr) {
    Serial.printf("[Weather] %s payload buffer alloc failed\n", sourceName);
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t readTotal = 0;
  uint32_t startMs = millis();
  bool readError = false;
  while (readTotal < (size_t)(bufSize - 1) && millis() - startMs < 15000) {
    if (!http.connected() && stream->available() == 0) break;
    size_t avail = stream->available();
    if (avail > 0) {
      int toRead = (int)min(avail, (size_t)(bufSize - 1 - readTotal));
      int r = stream->readBytes(rawBuf + readTotal, toRead);
      if (r <= 0) { readError = true; break; }
      readTotal += r;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5)); // yield -- the critical fix
    }
  }
  rawBuf[readTotal] = '\0';

  if (readError) {
    Serial.printf("[Weather] %s payload read error\n", sourceName);
    free(rawBuf);
    return false;
  }

  outPayload = String(rawBuf);
  free(rawBuf);
  return true;
}

// Same "days from civil" UTC conversion as astro_seeing_service.cpp's
// utcTmToUnix() (this toolchain has no timegm(), and mktime() assumes
// local time) -- duplicated locally for the same reason as the read-loop
// helper above. Open-Meteo's hourly timestamps are always on the hour
// (":00" minutes), so no minutes parameter is needed here either.
static uint32_t utcTmToUnix(int year, int month, int day, int hour) {
  int y = year;
  int m = month;
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  int yoe = (int)(y - era * 400);
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long daysSinceEpoch = era * 146097L + doe - 719468L;
  return (uint32_t)(daysSinceEpoch * 86400L + (long)hour * 3600L);
}

// 24-hour precipitation-probability forecast from Open-Meteo, used for
// the Weather page's hourly strip. forecast_days=2 (rather than 1) so
// there's always a full 24-hour rolling window available even late in
// the day, when "today" alone wouldn't have 24 hours left in it.
static void fetchHourlyPrecip() {
  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast?latitude=%f&longitude=%f"
    "&hourly=precipitation_probability&forecast_days=2&timezone=UTC",
    (double)HOME_LAT, (double)HOME_LON);

  http.begin(url);
  // Same useHTTP10 fix required for every manual-read-loop JSON fetch in
  // this project -- Open-Meteo can serve chunked transfer encoding on
  // larger responses, which corrupts a raw stream read if not forced to
  // HTTP/1.0.
  http.useHTTP10(true);
  int code = http.GET();
  g_precipHourlyLastHttpCode = code;
  if (code != 200) {
    Serial.printf("[Weather] Precip HTTP %d\n", code);
    http.end();
    return;
  }

  String payload;
  if (!readHttpBodySafely(http, payload, "Precip")) {
    http.end();
    return;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[Weather] Precip JSON parse error: %s\n", err.c_str());
    return;
  }

  JsonArray times = doc["hourly"]["time"].as<JsonArray>();
  JsonArray probs = doc["hourly"]["precipitation_probability"].as<JsonArray>();

  // times[] starts at hour 0 of "today" (UTC, per timezone=UTC above) --
  // start at the current UTC hour and take the next PRECIP_HOURLY_POINTS
  // entries as a rolling "next 24 hours" window, same same-day hour
  // lookup approach as fetchUvIndex() above.
  time_t now = time(nullptr);
  struct tm* utcNow = gmtime(&now);
  int startIdx = utcNow->tm_hour;
  if (startIdx < 0 || startIdx >= (int)times.size()) startIdx = 0;

  g_precipHourlyCount = 0;
  for (int i = startIdx;
       i < (int)times.size() && i < (int)probs.size() && g_precipHourlyCount < PRECIP_HOURLY_POINTS;
       i++) {
    String tStr = times[i].as<String>();
    int yr, mo, dy, hr, mi;
    if (sscanf(tStr.c_str(), "%d-%d-%dT%d:%d", &yr, &mo, &dy, &hr, &mi) == 5) {
      g_precipHourly[g_precipHourlyCount].unixTime = utcTmToUnix(yr, mo, dy, hr);
    } else {
      g_precipHourly[g_precipHourlyCount].unixTime = 0;
    }
    g_precipHourly[g_precipHourlyCount].precipProb = probs[i] | 0;
    g_precipHourlyCount++;
  }
  g_precipHourlyValid = (g_precipHourlyCount > 0);
}

void weather_service_update() {
  if (!wifi_manager_is_connected()) return;
  // 3 heavy HTTPS/JSON calls in a row were stacking with zero breathing
  // room between them -- same root cause as the original cross-service
  // flicker bug (heavy fetches disrupting the RGB panel's DMA timing),
  // just self-contained inside this one function instead of across
  // separate services, so networkTask's heavyFetchThisCycle guard never
  // saw it. Same vTaskDelay(200) "let the display catch its breath"
  // pattern used elsewhere (air quality, astro) added between each call.
  fetchCurrentConditions();
  vTaskDelay(pdMS_TO_TICKS(200));
  fetchForecast();
  vTaskDelay(pdMS_TO_TICKS(200));
  fetchUvIndex();
  vTaskDelay(pdMS_TO_TICKS(200));
  fetchHourlyPrecip();
}
