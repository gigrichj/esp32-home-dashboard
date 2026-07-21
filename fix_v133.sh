#!/usr/bin/env bash
set -e

python3 << 'PYEOF'
# --- Patch main.cpp: decouple astro retry timing ---
path = "src/main.cpp"
with open(path) as f:
    content = f.read()

old1 = '''static const uint32_t ASTRO_POLL_MS        = 40UL * 60UL * 1000UL; // seeing forecasts change slowly;
                                                                     // also deliberately offset from
                                                                     // the other poll intervals.'''
assert content.count(old1) == 1, f"expected 1, found {content.count(old1)}"
new1 = '''static const uint32_t ASTRO_POLL_MS        = 30UL * 60UL * 1000UL; // seeing forecasts change slowly;
                                                                     // also deliberately offset from
                                                                     // the other poll intervals.
static const uint32_t ASTRO_RETRY_MS       = 60UL * 1000UL; // 60s retry cadence
                                                                     // until the first successful fetch,
                                                                     // then settles to ASTRO_POLL_MS --
                                                                     // same pattern used for ISS crew count.'''
content = content.replace(old1, new1)

old2 = '''  uint32_t lastWeather = 0, lastAviation = 0, lastIss = 0, lastAirQuality = 0, lastAstro = 0;'''
assert content.count(old2) == 1, f"expected 1, found {content.count(old2)}"
new2 = '''  uint32_t lastWeather = 0, lastAviation = 0, lastIss = 0, lastAirQuality = 0, lastAstro = 0;
  bool astroDataLoaded = false;'''
content = content.replace(old2, new2)

old3 = '''    if (now - lastAstro > ASTRO_POLL_MS) {
      lastAstro = now;
      debug_log("astro fetch start");
      astro_seeing_service_update();
      debug_log("astro fetch done");
      vTaskDelay(pdMS_TO_TICKS(200)); // let the display catch its breath
      heavyFetchThisCycle = true;
    }'''
assert content.count(old3) == 1, f"expected 1, found {content.count(old3)}"
new3 = '''    uint32_t astroInterval = astroDataLoaded ? ASTRO_POLL_MS : ASTRO_RETRY_MS;
    if (now - lastAstro > astroInterval) {
      lastAstro = now;
      debug_log("astro fetch start");
      astro_seeing_service_update();
      if (g_astroLastHttpCode == 200 && g_astroForecastCount > 0) {
        astroDataLoaded = true;
      }
      debug_log("astro fetch done");
      vTaskDelay(pdMS_TO_TICKS(200)); // let the display catch its breath
      heavyFetchThisCycle = true;
    }'''
content = content.replace(old3, new3)

with open(path, "w") as f:
    f.write(content)
print("main.cpp: astro now retries every 60s until first success, then settles to 30 min")

# --- Patch astro_seeing_service.cpp: safe manual read loop, same as v128 ---
path2 = "src/services/astro_seeing_service.cpp"
with open(path2) as f:
    content2 = f.read()

old4 = '''  int code = http.GET();
  g_astroLastHttpCode = code;
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;'''
assert content2.count(old4) == 1, f"expected 1, found {content2.count(old4)}"
new4 = '''  int code = http.GET();
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
    JsonDocument doc;'''
content2 = content2.replace(old4, new4)

if "#include <esp_heap_caps.h>" not in content2:
    lines = content2.split("\n")
    for i, line in enumerate(lines):
        if line.startswith("#include"):
            lines.insert(i + 1, "#include <esp_heap_caps.h>")
            break
    content2 = "\n".join(lines)
if "#include <WiFiClient.h>" not in content2:
    lines = content2.split("\n")
    for i, line in enumerate(lines):
        if line.startswith("#include"):
            lines.insert(i + 1, "#include <WiFiClient.h>")
            break
    content2 = "\n".join(lines)

with open(path2, "w") as f:
    f.write(content2)
print("astro_seeing_service.cpp: replaced http.getString() with yield-safe manual read loop")

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
