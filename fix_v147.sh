#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/screens/screen_manager.cpp"
with open(path) as f:
    content = f.read()

old = '''static void drawDashboardBackground() {
  uint32_t t = millis();

  // Cloud drift low on the screen, echoing whatever the weather page is
  // currently showing, so the dashboard feels alive without being busy.
  if (g_weather.valid && g_weather.weatherId > 800) {
    uint16_t cloudColor = screen.color565(22, 26, 32);
    for (int i = 0; i < 4; i++) {
      int driftSpan = WIDTH + 200;
      uint32_t speed = 35 + (uint32_t)(i % 2) * 15;
      int x = (int)((t / speed + (uint32_t)i * 220) % (uint32_t)driftSpan) - 100;
      int y = 320 + (i % 2) * 50;
      drawCloudIcon(x, y, 26 + (i % 2) * 8, cloudColor);
    }
  }'''

assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"

new = '''static void drawDashboardBackground() {
  uint32_t t = millis();

  // Full weather-reactive background (rain, snow, sun/moon, thunderstorm
  // flashes, clouds) -- the same effects used on the Weather page, so the
  // Dashboard reflects current conditions instead of a generic low-key
  // cloud drift. isNight computed the same way draw_weather() does.
  if (g_weather.valid) {
    bool isNight = false;
    time_t nowTime = time(nullptr);
    if (nowTime > 100000 && g_weather.sunriseUnix > 0 && g_weather.sunsetUnix > 0) {
      isNight = (uint32_t)nowTime < g_weather.sunriseUnix || (uint32_t)nowTime > g_weather.sunsetUnix;
    }
    drawWeatherBackground(g_weather.weatherId, isNight);
  }'''

content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Replaced Dashboard's simple cloud drift with the full weather-reactive background from the Weather page")

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
