#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/services/astro_seeing_service.cpp"
with open(path) as f:
    content = f.read()

old = '''static bool fetchOpenMeteoFallback() {
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
  }'''
assert content.count(old) == 1, f"expected 1, found {content.count(old)}"

new = '''static bool fetchOpenMeteoFallback() {
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
  // Expose this fallback's own HTTP code on the Debug tab -- previously
  // g_astroLastHttpCode only reflected 7Timer's result (or a hardcoded 200
  // on fallback success), so a failing fallback was invisible: the on-screen
  // diagnostic kept showing 7Timer's stale failure code with no way to tell
  // whether the fallback was even being attempted, let alone what it got back.
  g_astroLastHttpCode = code;

  if (code != 200) {
    Serial.printf("[Astro] Open-Meteo fallback HTTP %d\\n", code);
    http.end();
    return false;
  }'''
content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Fixed g_astroLastHttpCode to reflect the Open-Meteo fallback's own result, not just 7Timer's")

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
