#include "aurora_service.h"
#include <WiFiClient.h>
#include <esp_heap_caps.h>
#include "wifi_manager.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "../state_mutex.h"

float g_currentKp = 0;
uint32_t g_currentKpPeriodEndUnix = 0;
bool g_kpObservedValid = false;

KpForecastPoint g_kpForecast[KP_FORECAST_MAX_POINTS];
int g_kpForecastCount = 0;
bool g_kpForecastValid = false;

int g_auroraLastHttpCode = -999;
String g_auroraLastFailureReason = "";

// Same safe-read pattern used across every other service in this project
// (astro_seeing_service.cpp, iss_service.cpp, etc.) -- reads in a loop
// with a yield on each empty-buffer pass so a stalled connection can't
// block long enough to trip the FreeRTOS task watchdog.
static bool readHttpBodySafely(HTTPClient& http, String& outPayload, const char* sourceName) {
  int payloadLen = http.getSize();
  int bufSize = (payloadLen > 0) ? payloadLen + 1 : 32768;
  char *rawBuf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
  if (rawBuf == nullptr) {
    Serial.printf("[Aurora] %s payload buffer alloc failed\n", sourceName);
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
    Serial.printf("[Aurora] %s payload read error\n", sourceName);
    free(rawBuf);
    return false;
  }

  outPayload = String(rawBuf);
  free(rawBuf);
  return true;
}

// Both NOAA SWPC endpoints return an array-of-arrays: a header row
// (["time_tag","Kp",...]) followed by data rows where every value is a
// JSON *string*, not a number (e.g. "3.33", not 3.33). Reading these with
// ArduinoJson's `| 0.0f` default-value pattern on a string field has bitten
// this project twice before (ISS max elevation, initially) with a silent
// type-mismatch returning the default instead of converting -- so here we
// deliberately read every numeric field as a string first and convert
// with atof(), which works regardless of how the value was encoded.
//
// time_tag format is "YYYY-MM-DD HH:MM:SS[.mmm]" in UTC (space-separated,
// not ISO8601 with a 'T'). Parsed manually and converted with timegm()
// since these are explicitly UTC timestamps, not local device time.
static uint32_t parseNoaaTimeTag(const char* tag) {
  if (tag == nullptr) return 0;
  struct tm t = {};
  int year, month, day, hour, minute, second;
  int matched = sscanf(tag, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
  if (matched < 6) return 0;
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = second;
  return (uint32_t)timegm(&t);
}

static bool fetchObservedKp() {
  HTTPClient http;
  http.begin("https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json");
  http.useHTTP10(true); // same chunked-transfer fix needed everywhere else in this project
  http.setTimeout(15000);
  int code = http.GET();
  state_lock();
  g_auroraLastHttpCode = code;
  state_unlock();
  if (code != 200) {
    Serial.printf("[Aurora] observed Kp HTTP %d\n", code);
    state_lock();
    g_auroraLastFailureReason = "Observed Kp: HTTP " + String(code);
    state_unlock();
    http.end();
    return false;
  }

  String payload;
  if (!readHttpBodySafely(http, payload, "observed Kp")) {
    http.end();
    state_lock();
    g_auroraLastFailureReason = "Observed Kp: payload read failed";
    state_unlock();
    return false;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[Aurora] observed Kp JSON parse error: %s\n", err.c_str());
    state_lock();
    g_auroraLastFailureReason = "Observed Kp: JSON parse: " + String(err.c_str());
    state_unlock();
    return false;
  }

  JsonArray rows = doc.as<JsonArray>();
  if (rows.size() < 2) {
    state_lock();
    g_auroraLastFailureReason = "Observed Kp: fewer than 2 rows (no data past header)";
    state_unlock();
    return false;
  }

  // Last row is the most recent observed period.
  JsonArray lastRow = rows[rows.size() - 1];
  const char* timeTag = lastRow[0] | "";
  const char* kpStr = lastRow[1] | "";

  state_lock();
  g_currentKp = atof(kpStr);
  g_currentKpPeriodEndUnix = parseNoaaTimeTag(timeTag) + (3 * 3600); // period END, not start
  g_kpObservedValid = true;
  state_unlock();
  return true;
}

static bool fetchForecastKp() {
  HTTPClient http;
  http.begin("https://services.swpc.noaa.gov/products/noaa-planetary-k-index-forecast.json");
  http.useHTTP10(true);
  http.setTimeout(15000);
  int code = http.GET();
  state_lock();
  g_auroraLastHttpCode = code;
  state_unlock();
  if (code != 200) {
    Serial.printf("[Aurora] Kp forecast HTTP %d\n", code);
    state_lock();
    g_auroraLastFailureReason = "Kp forecast: HTTP " + String(code);
    state_unlock();
    http.end();
    return false;
  }

  String payload;
  if (!readHttpBodySafely(http, payload, "Kp forecast")) {
    http.end();
    state_lock();
    g_auroraLastFailureReason = "Kp forecast: payload read failed";
    state_unlock();
    return false;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[Aurora] Kp forecast JSON parse error: %s\n", err.c_str());
    state_lock();
    g_auroraLastFailureReason = "Kp forecast: JSON parse: " + String(err.c_str());
    state_unlock();
    return false;
  }

  JsonArray rows = doc.as<JsonArray>();
  // Locked for the whole loop, same reasoning as every other array-fill
  // loop in this project (iss_service.cpp etc.) -- g_kpForecastCount must
  // never be visible to a reader implying more entries than written yet.
  state_lock();
  g_kpForecastCount = 0;
  // Row 0 is the header ("time_tag","kp",...) -- skip it.
  for (size_t i = 1; i < rows.size(); i++) {
    if (g_kpForecastCount >= KP_FORECAST_MAX_POINTS) break;
    JsonArray row = rows[i];
    const char* timeTag = row[0] | "";
    const char* kpStr = row[1] | "";
    g_kpForecast[g_kpForecastCount].periodStartUnix = parseNoaaTimeTag(timeTag);
    g_kpForecast[g_kpForecastCount].kp = atof(kpStr);
    g_kpForecastCount++;
  }
  g_kpForecastValid = (g_kpForecastCount > 0);
  if (!g_kpForecastValid) {
    g_auroraLastFailureReason = "Kp forecast: parsed OK but 0 rows after header";
  }
  state_unlock();
  return g_kpForecastValid;
}

void aurora_service_update() {
  if (!wifi_manager_is_connected()) return;
  fetchObservedKp();
  fetchForecastKp();
}

const char* aurora_visibility_label(float kp) {
  if (kp >= 8.0f) return "Likely";
  if (kp >= 7.0f) return "Possible";
  if (kp >= 6.0f) return "Slight Chance";
  return "Unlikely";
}
