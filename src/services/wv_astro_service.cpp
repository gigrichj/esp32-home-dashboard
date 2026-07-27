#include "wv_astro_service.h"
#include <WiFiClient.h>
#include <esp_heap_caps.h>
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "../state_mutex.h"

WvAstroForecastPoint g_wvAstroForecast[WV_ASTRO_MAX_POINTS];
int g_wvAstroForecastCount = 0;

WvCloudOnlyNight g_wvCloudOnlyNights[WV_CLOUD_ONLY_NIGHTS];
bool g_wvCloudOnlyValid = false;

int g_wvAstroLastHttpCode = -999;
String g_wvAstroLastFailureReason = "";

// Same Howard Hinnant civil-calendar algorithm as astro_seeing_service.cpp's
// utcTmToUnix() -- duplicated here since that one is file-scoped (static).
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

// Identical safe-read pattern used everywhere else in this project --
// reads in a loop with a yield on each empty-buffer pass so a stalled
// connection can't block long enough to trip the FreeRTOS task watchdog.
static bool readHttpBodySafely(HTTPClient& http, String& outPayload, const char* sourceName) {
  int payloadLen = http.getSize();
  int bufSize = (payloadLen > 0) ? payloadLen + 1 : 32768;
  char *rawBuf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
  if (rawBuf == nullptr) {
    Serial.printf("[WV Astro] %s payload buffer alloc failed\n", sourceName);
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
    Serial.printf("[WV Astro] %s payload read error\n", sourceName);
    free(rawBuf);
    return false;
  }

  outPayload = String(rawBuf);
  free(rawBuf);
  return true;
}

static bool fetch7TimerWV() {
  HTTPClient http;
  char url[192];
  snprintf(url, sizeof(url),
    "https://www.7timer.info/bin/api.pl?lon=%f&lat=%f&product=astro&output=json",
    (double)WV_LON, (double)WV_LAT);

  http.begin(url);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "ESP32-Home-Dashboard/1.0");
  http.useHTTP10(true);
  int code = http.GET();
  state_lock();
  g_wvAstroLastHttpCode = code;
  state_unlock();

  if (code != 200) {
    Serial.printf("[WV Astro] 7Timer HTTP %d\n", code);
    state_lock();
    g_wvAstroLastFailureReason = "7Timer: HTTP " + String(code);
    state_unlock();
    http.end();
    return false;
  }

  String payload;
  if (!readHttpBodySafely(http, payload, "7Timer")) {
    state_lock();
    g_wvAstroLastFailureReason = "7Timer: payload read failed";
    state_unlock();
    http.end();
    return false;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[WV Astro] 7Timer JSON parse error: %s\n", err.c_str());
    state_lock();
    g_wvAstroLastFailureReason = "7Timer: JSON parse: " + String(err.c_str());
    state_unlock();
    return false;
  }

  const char* initStr = doc["init"] | "";
  uint32_t initUnix = 0;
  if (strlen(initStr) >= 10) {
    char yearBuf[5] = {0}, monBuf[3] = {0}, dayBuf[3] = {0}, hourBuf[3] = {0};
    memcpy(yearBuf, initStr, 4);
    memcpy(monBuf, initStr + 4, 2);
    memcpy(dayBuf, initStr + 6, 2);
    memcpy(hourBuf, initStr + 8, 2);
    int year  = atoi(yearBuf);
    int month = atoi(monBuf);
    int day   = atoi(dayBuf);
    int hour  = atoi(hourBuf);
    initUnix = utcTmToUnix(year, month, day, hour);
  }

  JsonArray series = doc["dataseries"].as<JsonArray>();
  // Locked for the whole loop, same reasoning as every other array-fill
  // loop in this project -- g_wvAstroForecastCount must never be visible
  // to a reader implying more points than have actually been written yet.
  state_lock();
  g_wvAstroForecastCount = 0;
  for (JsonObject p : series) {
    if (g_wvAstroForecastCount >= WV_ASTRO_MAX_POINTS) break;
    int timepoint = p["timepoint"] | 0;
    WvAstroForecastPoint& pt = g_wvAstroForecast[g_wvAstroForecastCount];
    pt.unixTime     = initUnix + (uint32_t)timepoint * 3600UL;
    pt.cloudcover   = p["cloudcover"]   | 0;
    pt.seeing       = p["seeing"]       | 0;
    pt.transparency = p["transparency"] | 0;
    pt.liftedindex  = p["liftedindex"]  | 0;
    const char* prectype = p["prectype"] | "none";
    pt.prectype = String(prectype);
    g_wvAstroForecastCount++;
  }
  if (g_wvAstroForecastCount == 0) {
    g_wvAstroLastFailureReason = "7Timer: parsed OK but 0 forecast points";
  }
  state_unlock();
  return g_wvAstroForecastCount > 0;
}

// Extends 2 more nights past 7Timer's 3-day window using Open-Meteo cloud
// cover only -- no seeing/transparency data exists this far out from any
// free source, so days 4-5 are deliberately lower-confidence and shown
// differently on the WV page. Night window approximated as 02:00-08:00
// UTC (~9pm-3am Eastern) -- doesn't shift with DST, treat as approximate.
static bool fetchOpenMeteoWvExtension() {
  HTTPClient http;
  char url[224];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast?latitude=%f&longitude=%f"
    "&hourly=cloudcover&forecast_days=5&timezone=UTC",
    (double)WV_LAT, (double)WV_LON);

  http.begin(url);
  http.setTimeout(15000);
  http.useHTTP10(true); // same chunked-transfer fix needed everywhere else
  int code = http.GET();
  state_lock();
  g_wvAstroLastHttpCode = code;
  state_unlock();

  if (code != 200) {
    Serial.printf("[WV Astro] Open-Meteo extension HTTP %d\n", code);
    state_lock();
    g_wvAstroLastFailureReason = "Open-Meteo ext: HTTP " + String(code);
    state_unlock();
    http.end();
    return false;
  }

  String payload;
  if (!readHttpBodySafely(http, payload, "Open-Meteo WV extension")) {
    state_lock();
    g_wvAstroLastFailureReason = "Open-Meteo ext: payload read failed";
    state_unlock();
    http.end();
    return false;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[WV Astro] Open-Meteo extension JSON parse error: %s\n", err.c_str());
    state_lock();
    g_wvAstroLastFailureReason = "Open-Meteo ext: JSON parse: " + String(err.c_str());
    state_unlock();
    return false;
  }

  JsonObject hourly = doc["hourly"];
  if (hourly.isNull()) {
    state_lock();
    g_wvAstroLastFailureReason = "Open-Meteo ext: missing 'hourly' object";
    state_unlock();
    return false;
  }

  JsonArray times = hourly["time"].as<JsonArray>();
  JsonArray cloudcovers = hourly["cloudcover"].as<JsonArray>();
  size_t total = times.size();

  if (total == 0) {
    state_lock();
    g_wvAstroLastFailureReason = "Open-Meteo ext: HTTP 200 but 0 hourly points";
    state_unlock();
    return false;
  }

  // 7Timer's 3-day window (days 0-2) is already covered by real
  // seeing/transparency data -- only hours from day index 3 onward are
  // used here. "Day index" is computed relative to this response's own
  // first timestamp (hour 0 of day 0), not current wall-clock time, so
  // it stays correct regardless of what hour this fetch happens to run.
  const char* firstTimeStr = times[0] | "";
  int fy, fm, fd, fh, fmin;
  sscanf(firstTimeStr, "%d-%d-%dT%d:%d", &fy, &fm, &fd, &fh, &fmin);
  uint32_t day0Start = utcTmToUnix(fy, fm, fd, 0);

  float nightSum[WV_CLOUD_ONLY_NIGHTS] = {0, 0};
  int nightCount[WV_CLOUD_ONLY_NIGHTS] = {0, 0};

  for (size_t i = 0; i < total; i++) {
    const char* timeStr = times[i] | "";
    int year, month, day, hour, minute;
    if (sscanf(timeStr, "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute) != 5) continue;
    uint32_t thisUnix = utcTmToUnix(year, month, day, hour);
    int dayIndex = (int)((thisUnix - day0Start) / 86400UL);
    int nightIdx = dayIndex - 3; // days 0-2 covered by 7Timer; night 0 = day index 3
    if (nightIdx < 0 || nightIdx >= WV_CLOUD_ONLY_NIGHTS) continue;
    if (hour < 2 || hour >= 8) continue; // approximate nighttime window (UTC)

    float cloudcoverPct = cloudcovers[i] | 0.0f;
    nightSum[nightIdx] += cloudcoverPct;
    nightCount[nightIdx]++;
  }

  state_lock();
  bool anyValid = false;
  for (int n = 0; n < WV_CLOUD_ONLY_NIGHTS; n++) {
    if (nightCount[n] > 0) {
      g_wvCloudOnlyNights[n].avgCloudcoverPct = nightSum[n] / nightCount[n];
      g_wvCloudOnlyNights[n].nightDateUnix = day0Start + (uint32_t)(n + 3) * 86400UL;
      anyValid = true;
    } else {
      g_wvCloudOnlyNights[n].avgCloudcoverPct = -1;
    }
  }
  g_wvCloudOnlyValid = anyValid;
  if (!anyValid) {
    g_wvAstroLastFailureReason = "Open-Meteo ext: no hours matched days 4-5 night window";
  }
  state_unlock();
  return anyValid;
}

void wv_astro_service_update() {
  if (!wifi_manager_is_connected()) return;
  // Both fetched every time -- unlike the home Astro page, this isn't an
  // either/or fallback. See wv_astro_service.h for why a 7Timer failure
  // here deliberately does NOT fall back to estimated seeing/transparency.
  fetch7TimerWV();
  fetchOpenMeteoWvExtension();
}
