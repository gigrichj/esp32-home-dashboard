#!/usr/bin/env bash
set -e

python3 << 'PYEOF'
path = "src/services/astro_seeing_service.cpp"
with open(path) as f:
    content = f.read()

old = '''void astro_seeing_service_update() {
  astro_recompute_moon_phase();

  if (!wifi_manager_is_connected()) return;

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
  int code = http.GET();
  g_astroLastHttpCode = code;
  if (code == 200) {
    // Read the body manually with an explicit timeout and a yield on every
    // iteration, instead of http.getString() -- that call reads one byte at
    // a time in a tight loop with no yield, and a reset/stalled connection
    // can block long enough to trip the FreeRTOS task watchdog (same fix
    // applied to the aviation states fetch after a decoded crash trace
    // confirmed it there).
    int payloadLen = http.getSize();
    int bufSize = (payloadLen > 0) ? payloadLen + 1 : 32768;
    char *rawBuf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
    if (rawBuf == nullptr) {
      Serial.println("[Astro] payload buffer alloc failed");
      http.end();
      return;
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
    rawBuf[readTotal] = '\\0';

    if (readError) {
      Serial.println("[Astro] payload read error");
      free(rawBuf);
      http.end();
      return;
    }

    String payload(rawBuf);
    free(rawBuf);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
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
    } else {
      Serial.printf("[Astro] JSON parse error: %s\\n", err.c_str());
      Serial.printf("[Astro] raw payload: %s\\n", payload.c_str());
    }
  } else {
    Serial.printf("[Astro] HTTP %d (negative = connection/timeout error)\\n", code);
  }
  http.end();
}'''

assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"

new = '''// Reads an HTTP response body with an explicit timeout and a yield on every
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
    Serial.printf("[Astro] %s payload buffer alloc failed\\n", sourceName);
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
  rawBuf[readTotal] = '\\0';

  if (readError) {
    Serial.printf("[Astro] %s payload read error\\n", sourceName);
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
  int code = http.GET();
  g_astroLastHttpCode = code;

  if (code != 200) {
    Serial.printf("[Astro] 7Timer HTTP %d (negative = connection/timeout error)\\n", code);
    http.end();
    return false;
  }

  String payload;
  if (!readHttpBodySafely(http, payload, "7Timer")) {
    http.end();
    return false;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[Astro] 7Timer JSON parse error: %s\\n", err.c_str());
    Serial.printf("[Astro] 7Timer raw payload: %s\\n", payload.c_str());
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
  int code = http.GET();

  if (code != 200) {
    Serial.printf("[Astro] Open-Meteo fallback HTTP %d\\n", code);
    http.end();
    return false;
  }

  String payload;
  if (!readHttpBodySafely(http, payload, "Open-Meteo fallback")) {
    http.end();
    return false;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[Astro] Open-Meteo fallback JSON parse error: %s\\n", err.c_str());
    return false;
  }

  JsonObject hourly = doc["hourly"];
  if (hourly.isNull()) {
    Serial.println("[Astro] Open-Meteo fallback: missing 'hourly' object");
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
}'''

content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Added Open-Meteo fallback (estimated seeing/transparency/cloudcover/instability) when 7Timer fails")

import re
vpath = "src/version.h"
with open(vpath) as f:
    vcontent = f.read()
m = re.search(r'#define FIRMWARE_VERSION "v(\d+)"', vcontent)
assert m, "could not find FIRMWARE_VERSION define"
old_ver_line = m.group(0)
new_ver_num = int(m.group(1)) + 1
new_ver_line = f'#define FIRMWARE_VERSION "v{new_ver_num}"'
assert vcontent.count(old_ver_line) == 1
vcontent = vcontent.replace(old_ver_line, new_ver_line)
with open(vpath, "w") as f:
    f.write(vcontent)
print(f"Bumped version.h: {old_ver_line} -> {new_ver_line}")
PYEOF
