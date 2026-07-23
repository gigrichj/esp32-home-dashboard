#include "astro_seeing_service.h"
#include <WiFiClient.h>
#include <esp_heap_caps.h>
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

AstroForecastPoint g_astroForecast[ASTRO_MAX_POINTS];
int g_astroForecastCount = 0;
int g_astroLastHttpCode = -999;
String g_astroLastFailureReason = "";

float g_moonPhaseFraction = 0;
float g_moonIllumPercent = 0;
float g_daysUntilNewMoon = 0;
String g_moonPhaseLabel = "--";

// Wording now mirrors the on-screen GOOD/FAIR/POOR/BAD color key exactly --
// same index thresholds as astroSeverityColor() in screen_manager.cpp, so a
// given index always shows a word that matches its color band.
const char* astro_seeing_label(int idx) {
  if (idx <= 2) return "GOOD";
  if (idx <= 4) return "FAIR";
  if (idx <= 6) return "POOR";
  if (idx <= 8) return "BAD";
  return "--";
}

const char* astro_transparency_label(int idx) {
  if (idx <= 2) return "GOOD";
  if (idx <= 4) return "FAIR";
  if (idx <= 6) return "POOR";
  if (idx <= 8) return "BAD";
  return "--";
}

const char* astro_cloudcover_label(int idx) {
  // Cloud cover's color scale uses a max of 9 (not 8), so the thresholds
  // are scaled up proportionally to land on the same 4 color bands.
  if (idx <= 2) return "GOOD";
  if (idx <= 4) return "FAIR";
  if (idx <= 6) return "POOR";
  if (idx <= 9) return "BAD";
  return "--";
}

const char* astro_instability_label(int liftedIndex) {
  if (liftedIndex > 0) return "Stable";
  if (liftedIndex > -4) return "Slight Risk";
  if (liftedIndex > -8) return "Moderate Risk";
  return "High Risk";
}

const char* astro_tonight_verdict(int cloudcover, int seeing, int transparency,
                                   float moonIllumPercent, float* outBadness) {
  float cloudFrac = (float)(cloudcover - 1) / 8.0f;    // cloudcover is 1-9
  float seeingFrac = (float)(seeing - 1) / 7.0f;       // seeing is 1-8
  float transFrac = (float)(transparency - 1) / 7.0f;  // transparency is 1-8
  float moonFrac = moonIllumPercent / 100.0f;

  float badness = (cloudFrac * 3.0f + seeingFrac * 2.0f + transFrac * 1.0f + moonFrac * 1.0f) / 7.0f;
  if (outBadness) *outBadness = badness;

  // Thresholds match the 0.25/0.5/0.75 bands used everywhere this badness
  // value is turned into a color, so the word and the color it's drawn in
  // always agree with the on-screen GOOD/FAIR/POOR/BAD key.
  if (badness < 0.25f) return "GOOD NIGHT";
  if (badness < 0.5f)  return "FAIR NIGHT";
  if (badness < 0.75f) return "POOR NIGHT";
  return "BAD NIGHT";
}

// ESP32's toolchain doesn't provide timegm(), and mktime() assumes local
// time (this device runs on EST5EDT, not UTC) -- so 7Timer's UTC "init"
// timestamp needs a timezone-independent manual conversion instead.
// This is the standard "days from civil" algorithm (Howard Hinnant),
// correct for the Gregorian calendar.
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

static void computeMoonPhase(uint32_t nowUnix) {
  const double synodicDays = 29.530588;
  // Known new moon reference: 2000-01-06 18:14 UTC
  const double refNewMoonUnix = 947182440.0;
  double daysSince = ((double)nowUnix - refNewMoonUnix) / 86400.0;
  double phaseDays = fmod(daysSince, synodicDays);
  if (phaseDays < 0) phaseDays += synodicDays;
  double frac = phaseDays / synodicDays;
  double illum = (1.0 - cos(2.0 * PI * frac)) / 2.0 * 100.0;

  g_moonPhaseFraction = (float)frac;
  g_moonIllumPercent = (float)illum;
  // Same phaseDays/synodicDays values already computed above -- next new
  // moon is however many days remain until phaseDays wraps back to 0.
  g_daysUntilNewMoon = (float)(synodicDays - phaseDays);

  if (frac < 0.03 || frac > 0.97)      g_moonPhaseLabel = "New Moon";
  else if (frac < 0.22)                g_moonPhaseLabel = "Waxing Crescent";
  else if (frac < 0.28)                g_moonPhaseLabel = "First Quarter";
  else if (frac < 0.47)                g_moonPhaseLabel = "Waxing Gibbous";
  else if (frac < 0.53)                g_moonPhaseLabel = "Full Moon";
  else if (frac < 0.72)                g_moonPhaseLabel = "Waning Gibbous";
  else if (frac < 0.78)                g_moonPhaseLabel = "Last Quarter";
  else                                  g_moonPhaseLabel = "Waning Crescent";
}

void astro_recompute_moon_phase() {
  uint32_t nowUnix = (uint32_t)time(nullptr);
  if (nowUnix > 100000) {
    computeMoonPhase(nowUnix);
  }
}

// Reads an HTTP response body with an explicit timeout and a yield on every
// iteration, instead of http.getString() -- that call reads one byte at a
// time in a tight loop with no yield, and a reset/stalled connection can
// block long enough to trip the FreeRTOS task watchdog (same fix applied
// to the aviation states fetch after a decoded crash trace confirmed it
// there). Shared by both the 7Timer fetch and the Open-Meteo fallback.
static bool readHttpBodySafely(HTTPClient& http, String& outPayload, const char* sourceName) {
  int payloadLen = http.getSize();
  int bufSize = (payloadLen > 0) ? payloadLen + 1 : 32768;
  char *rawBuf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
  if (rawBuf == nullptr) {
    Serial.printf("[Astro] %s payload buffer alloc failed\n", sourceName);
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
    Serial.printf("[Astro] %s payload read error\n", sourceName);
    free(rawBuf);
    return false;
  }

  outPayload = String(rawBuf);
  free(rawBuf);
  return true;
}

static bool fetch7Timer() {
  HTTPClient http;
  char url[192];
  snprintf(url, sizeof(url),
    "https://www.7timer.info/bin/api.pl?lon=%f&lat=%f&product=astro&output=json",
    (double)HOME_LON, (double)HOME_LAT);

  http.begin(url);
  http.setTimeout(15000); // 7Timer is a small community-run service and
                          // can be slow to respond -- the default timeout
                          // was likely cutting it off before it replied.
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); // 7Timer returns a
                          // 302 redirect (likely to a mirror server) --
                          // HTTPClient doesn't follow redirects by default.
  http.addHeader("User-Agent", "ESP32-Home-Dashboard/1.0");
  // Same useHTTP10 fix as every other manual-read-loop JSON fetch in this
  // project -- this one had never been converted, and was seen returning
  // a raw unparsed chunked-transfer-encoding header ("14ed" hex chunk
  // size) wrapped around otherwise-valid JSON, the same silent
  // DeserializationError::InvalidInput signature hit before with
  // Open-Meteo, ISS TLE, and UV index.
  http.useHTTP10(true);
  int code = http.GET();
  g_astroLastHttpCode = code;

  if (code != 200) {
    Serial.printf("[Astro] 7Timer HTTP %d (negative = connection/timeout error)\n", code);
    g_astroLastFailureReason = "7Timer: HTTP " + String(code);
    http.end();
    return false;
  }

  String payload;
  if (!readHttpBodySafely(http, payload, "7Timer")) {
    g_astroLastFailureReason = "7Timer: payload read failed";
    http.end();
    return false;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[Astro] 7Timer JSON parse error: %s\n", err.c_str());
    Serial.printf("[Astro] 7Timer raw payload: %s\n", payload.c_str());
    g_astroLastFailureReason = "7Timer: JSON parse: " + String(err.c_str());
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
  g_astroForecastCount = 0;
  for (JsonObject p : series) {
    if (g_astroForecastCount >= ASTRO_MAX_POINTS) break;
    int timepoint = p["timepoint"] | 0;
    AstroForecastPoint& pt = g_astroForecast[g_astroForecastCount];
    pt.unixTime     = initUnix + (uint32_t)timepoint * 3600UL;
    pt.cloudcover   = p["cloudcover"]   | 0;
    pt.seeing       = p["seeing"]       | 0;
    pt.transparency = p["transparency"] | 0;
    pt.liftedindex  = p["liftedindex"]  | 0;
    const char* prectype = p["prectype"] | "none";
    pt.prectype = String(prectype);
    g_astroForecastCount++;
  }

  if (g_astroForecastCount == 0) {
    g_astroLastFailureReason = "7Timer: parsed OK but 0 forecast points";
  }
  return g_astroForecastCount > 0;
}

// Fallback used only when 7Timer is unreachable. Open-Meteo (free, no auth
// key) has no dedicated astro-seeing product, so seeing/transparency/
// instability here are ESTIMATED from raw meteorological fields using
// simple heuristics -- not the same physics-based model 7Timer uses.
// Good enough to keep the Astro page populated during an outage; treat
// as approximate, not authoritative.
static int estimateSeeingFromWind(float windKmh) {
  // Higher near-surface wind is used as a rough proxy for turbulence.
  // This is a simplification -- real seeing is driven mainly by upper-
  // atmosphere jet stream wind shear, which this free tier doesn't expose.
  int idx = 1 + (int)(windKmh / 5.0f);
  if (idx < 1) idx = 1;
  if (idx > 8) idx = 8;
  return idx;
}

static int estimateTransparencyFromHumidity(float humidityPct) {
  int idx = 1 + (int)round((humidityPct / 100.0f) * 7.0f);
  if (idx < 1) idx = 1;
  if (idx > 8) idx = 8;
  return idx;
}

static int estimateCloudcoverIndex(float cloudcoverPct) {
  int idx = 1 + (int)round((cloudcoverPct / 100.0f) * 8.0f);
  if (idx < 1) idx = 1;
  if (idx > 9) idx = 9;
  return idx;
}

static int estimateLiftedIndexFromCape(float cape) {
  // Rough anti-correlation: higher CAPE (more energy available for storms)
  // maps to a more negative lifted-index-like value (more unstable).
  int li = 10 - (int)(cape / 100.0f);
  if (li < -10) li = -10;
  if (li > 15) li = 15;
  return li;
}

static bool fetchOpenMeteoFallback() {
  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
    "https://api.open-meteo.com/v1/forecast?latitude=%f&longitude=%f"
    "&hourly=cloudcover,cape,windspeed_10m,relativehumidity_2m"
    "&forecast_days=2&timezone=UTC",
    (double)HOME_LAT, (double)HOME_LON);

  http.begin(url);
  http.setTimeout(15000);
  // Force HTTP/1.0 -- Open-Meteo's larger response was suspected to arrive
  // chunked (Transfer-Encoding: chunked, no Content-Length), and our manual
  // raw-stream read loop (readHttpBodySafely, used since v128) doesn't
  // strip chunk-size framing the way http.getString() would -- resulting
  // in "invalid input" JSON parse errors even though the underlying data
  // was valid. HTTP/1.0 has no chunked transfer mechanism, so servers
  // typically respond with a plain Content-Length body instead.
  http.useHTTP10(true);
  int code = http.GET();
  // Expose this fallback's own HTTP code on the Debug tab -- previously
  // g_astroLastHttpCode only reflected 7Timer's result (or a hardcoded 200
  // on fallback success), so a failing fallback was invisible: the on-screen
  // diagnostic kept showing 7Timer's stale failure code with no way to tell
  // whether the fallback was even being attempted, let alone what it got back.
  g_astroLastHttpCode = code;

  if (code != 200) {
    Serial.printf("[Astro] Open-Meteo fallback HTTP %d\n", code);
    g_astroLastFailureReason = "Open-Meteo: HTTP " + String(code);
    http.end();
    return false;
  }

  String payload;
  if (!readHttpBodySafely(http, payload, "Open-Meteo fallback")) {
    g_astroLastFailureReason = "Open-Meteo: payload read failed";
    http.end();
    return false;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[Astro] Open-Meteo fallback JSON parse error: %s\n", err.c_str());
    g_astroLastFailureReason = "Open-Meteo: JSON parse: " + String(err.c_str());
    return false;
  }

  JsonObject hourly = doc["hourly"];
  if (hourly.isNull()) {
    Serial.println("[Astro] Open-Meteo fallback: missing 'hourly' object");
    g_astroLastFailureReason = "Open-Meteo: response missing 'hourly' object";
    return false;
  }

  JsonArray times = hourly["time"].as<JsonArray>();
  JsonArray cloudcovers = hourly["cloudcover"].as<JsonArray>();
  JsonArray capes = hourly["cape"].as<JsonArray>();
  JsonArray winds = hourly["windspeed_10m"].as<JsonArray>();
  JsonArray humidities = hourly["relativehumidity_2m"].as<JsonArray>();

  size_t total = times.size();
  g_astroForecastCount = 0;
  // Sample every 3rd hour to match 7Timer's 3-hour spacing / ASTRO_MAX_POINTS.
  for (size_t i = 0; i < total && g_astroForecastCount < ASTRO_MAX_POINTS; i += 3) {
    const char* timeStr = times[i] | "";
    int year, month, day, hour, minute;
    if (sscanf(timeStr, "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute) != 5) {
      continue;
    }

    float cloudcoverPct = cloudcovers[i] | 0.0f;
    float cape = capes[i] | 0.0f;
    float windKmh = winds[i] | 0.0f;
    float humidityPct = humidities[i] | 0.0f;

    AstroForecastPoint& pt = g_astroForecast[g_astroForecastCount];
    pt.unixTime     = utcTmToUnix(year, month, day, hour);
    pt.cloudcover   = estimateCloudcoverIndex(cloudcoverPct);
    pt.seeing       = estimateSeeingFromWind(windKmh);
    pt.transparency = estimateTransparencyFromHumidity(humidityPct);
    pt.liftedindex  = estimateLiftedIndexFromCape(cape);
    pt.prectype     = "none"; // not available from this endpoint without
                              // an extra weathercode lookup; not critical.
    g_astroForecastCount++;
  }

  if (g_astroForecastCount > 0) {
    g_astroLastHttpCode = 200; // fallback succeeded; on-screen diagnostic
                               // still reads as a healthy fetch.
    g_astroLastFailureReason = "";
  } else {
    g_astroLastFailureReason = "Open-Meteo: HTTP 200 but 0 forecast points (times.size()=" + String(times.size()) + ")";
  }

  return g_astroForecastCount > 0;
}

void astro_seeing_service_update() {
  astro_recompute_moon_phase();

  if (!wifi_manager_is_connected()) return;

  if (fetch7Timer()) {
    return;
  }

  Serial.println("[Astro] 7Timer failed, falling back to Open-Meteo (estimated seeing/transparency)");
  fetchOpenMeteoFallback();
}
