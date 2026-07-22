#!/usr/bin/env bash
set -e

python3 << 'EOF'
# --- Add a shared diagnostic string to the header ---
hpath = "src/services/astro_seeing_service.h"
with open(hpath) as f:
    hcontent = f.read()

old_h = '''extern int g_astroLastHttpCode;'''
assert hcontent.count(old_h) == 1, f"expected 1, found {hcontent.count(old_h)}"
new_h = '''extern int g_astroLastHttpCode;

// Human-readable reason for the most recent fetch/parse failure (either
// source), shown on-screen next to the HTTP code -- since serial logging
// isn't always reliably captured, this gives on-device visibility into
// exactly which step failed (JSON parse, missing field, etc.) without
// needing a serial monitor.
extern String g_astroLastFailureReason;'''
hcontent = hcontent.replace(old_h, new_h)

with open(hpath, "w") as f:
    f.write(hcontent)
print("Added g_astroLastFailureReason to header")

# --- Populate it at every failure point in the .cpp ---
path = "src/services/astro_seeing_service.cpp"
with open(path) as f:
    content = f.read()

old1 = '''int g_astroLastHttpCode = -999;'''
assert content.count(old1) == 1
new1 = '''int g_astroLastHttpCode = -999;
String g_astroLastFailureReason = "";'''
content = content.replace(old1, new1)

# 7Timer failure points
content = content.replace(
    '''  if (code != 200) {
    Serial.printf("[Astro] 7Timer HTTP %d (negative = connection/timeout error)\\n", code);
    http.end();
    return false;
  }''',
    '''  if (code != 200) {
    Serial.printf("[Astro] 7Timer HTTP %d (negative = connection/timeout error)\\n", code);
    g_astroLastFailureReason = "7Timer: HTTP " + String(code);
    http.end();
    return false;
  }'''
)

content = content.replace(
    '''  String payload;
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
  }''',
    '''  String payload;
  if (!readHttpBodySafely(http, payload, "7Timer")) {
    g_astroLastFailureReason = "7Timer: payload read failed";
    http.end();
    return false;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[Astro] 7Timer JSON parse error: %s\\n", err.c_str());
    Serial.printf("[Astro] 7Timer raw payload: %s\\n", payload.c_str());
    g_astroLastFailureReason = "7Timer: JSON parse: " + String(err.c_str());
    return false;
  }'''
)

content = content.replace(
    '''  return g_astroForecastCount > 0;
}

// Fallback used only when 7Timer is unreachable.''',
    '''  if (g_astroForecastCount == 0) {
    g_astroLastFailureReason = "7Timer: parsed OK but 0 forecast points";
  }
  return g_astroForecastCount > 0;
}

// Fallback used only when 7Timer is unreachable.'''
)

# Open-Meteo fallback failure points
content = content.replace(
    '''  if (code != 200) {
    Serial.printf("[Astro] Open-Meteo fallback HTTP %d\\n", code);
    http.end();
    return false;
  }''',
    '''  if (code != 200) {
    Serial.printf("[Astro] Open-Meteo fallback HTTP %d\\n", code);
    g_astroLastFailureReason = "Open-Meteo: HTTP " + String(code);
    http.end();
    return false;
  }'''
)

content = content.replace(
    '''  String payload;
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
  }''',
    '''  String payload;
  if (!readHttpBodySafely(http, payload, "Open-Meteo fallback")) {
    g_astroLastFailureReason = "Open-Meteo: payload read failed";
    http.end();
    return false;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[Astro] Open-Meteo fallback JSON parse error: %s\\n", err.c_str());
    g_astroLastFailureReason = "Open-Meteo: JSON parse: " + String(err.c_str());
    return false;
  }

  JsonObject hourly = doc["hourly"];
  if (hourly.isNull()) {
    Serial.println("[Astro] Open-Meteo fallback: missing 'hourly' object");
    g_astroLastFailureReason = "Open-Meteo: response missing 'hourly' object";
    return false;
  }'''
)

content = content.replace(
    '''  if (g_astroForecastCount > 0) {
    g_astroLastHttpCode = 200; // fallback succeeded; on-screen diagnostic
                               // still reads as a healthy fetch.
  }

  return g_astroForecastCount > 0;
}''',
    '''  if (g_astroForecastCount > 0) {
    g_astroLastHttpCode = 200; // fallback succeeded; on-screen diagnostic
                               // still reads as a healthy fetch.
    g_astroLastFailureReason = "";
  } else {
    g_astroLastFailureReason = "Open-Meteo: HTTP 200 but 0 forecast points (times.size()=" + String(times.size()) + ")";
  }

  return g_astroForecastCount > 0;
}'''
)

with open(path, "w") as f:
    f.write(content)
print("Populated g_astroLastFailureReason at every failure/short-circuit point in both fetch paths")

# --- Draw it on the Astro page, right next to the HTTP result line ---
sm_path = "src/screens/screen_manager.cpp"
with open(sm_path) as f:
    sm = f.read()

old_draw = '''    char httpLine[48];
    snprintf(httpLine, sizeof(httpLine), "Last HTTP result: %d", g_astroLastHttpCode);
    screen.drawString(httpLine, 460, 418);
    screen.drawString("(-999=never tried, neg=connection error)", 460, 446);'''
assert sm.count(old_draw) == 1, f"expected 1, found {sm.count(old_draw)}"
new_draw = '''    char httpLine[48];
    snprintf(httpLine, sizeof(httpLine), "Last HTTP result: %d", g_astroLastHttpCode);
    screen.drawString(httpLine, 460, 418);
    screen.drawString("(-999=never tried, neg=connection error)", 460, 446);
    screen.setTextSize(1);
    screen.drawString(g_astroLastFailureReason, 460, 470);'''
sm = sm.replace(old_draw, new_draw)

with open(sm_path, "w") as f:
    f.write(sm)
print("Added g_astroLastFailureReason readout to the Astro page's 'No astro data yet' screen")

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
EOF
