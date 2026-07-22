#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/screens/screen_manager.cpp"
with open(path) as f:
    content = f.read()

old = '''static void draw_debug() {
  screen.setTextDatum(textdatum_t::top_left);

  char bbLine[48];
  snprintf(bbLine, sizeof(bbLine), "Bounce buffer: %d lines", PanelDisplay::getBounceBufferLines());
  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString(bbLine, 10, 50);
  screen.setTextSize(1);
  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Tap NEXT to save the next test value and reboot", 10, 78);

  screen.fillRect(600, 400, 180, 60, colorAccent);
  screen.setTextColor(colorBg, colorAccent);
  screen.setTextSize(2);
  screen.setTextDatum(textdatum_t::middle_center);
  screen.drawString("NEXT >>", 690, 430);
  screen.setTextDatum(textdatum_t::top_left);

  char pollLine[48];
  snprintf(pollLine, sizeof(pollLine), "Aviation poll: %lus", (unsigned long)(g_aviationPollMs / 1000));
  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString(pollLine, 10, 320);
  screen.fillRect(230, 310, 180, 60, colorAccent);
  screen.setTextColor(colorBg, colorAccent);
  screen.setTextDatum(textdatum_t::middle_center);
  screen.drawString("NEXT >>", 320, 340);
  screen.setTextDatum(textdatum_t::top_left);

  char heapLine[64];
  snprintf(heapLine, sizeof(heapLine), "Free heap: %u  Free PSRAM: %u",
           static_cast<unsigned>(ESP.getFreeHeap()),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString(heapLine, 10, 130);

  char largestBlockLine[64];
  snprintf(largestBlockLine, sizeof(largestBlockLine), "Largest free block (8BIT): %u",
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString(largestBlockLine, 10, 160);

  // TEMP DEBUG (v114): clear sunrise/sunset readout, moved here from the
  // Weather page so it is not crowded by other data. Remove once root
  // cause of sunrise/sunset being wrong is confirmed and fixed.
  {
    char srLine[64];
    char ssLine[64];
    char debugSunriseBuf[16];
    char debugSunsetBuf[16];
    formatHHMM(g_weather.sunriseUnix, debugSunriseBuf, sizeof(debugSunriseBuf));
    formatHHMM(g_weather.sunsetUnix, debugSunsetBuf, sizeof(debugSunsetBuf));
    snprintf(srLine, sizeof(srLine), "SUNRISE %s UNIX %lu",
             debugSunriseBuf,
             (unsigned long)g_weather.sunriseUnix);
    snprintf(ssLine, sizeof(ssLine), "SUNSET %s UNIX %lu",
             debugSunsetBuf,
             (unsigned long)g_weather.sunsetUnix);
    screen.setTextSize(2);
    screen.setTextColor(colorAccent, colorBg);
    screen.drawString(srLine, 400, 50);
    screen.drawString(ssLine, 400, 78);
  }

  screen.setTextSize(1);
  screen.setTextColor(colorText, colorBg);
  int y = 105;
  for (int i = 0; i < DEBUG_LOG_LINES; i++) {
    if (g_debugLog[i].length() > 0) {
      screen.drawString(g_debugLog[i], 10, y);
    }
    y += 18;
  }
}'''

assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"

new = '''// A simple, original decorative badge for the Debug page -- a generic
// heraldic shield silhouette (rounded top, tapering sides to a point;
// a common, non-branded shape used widely, not matching any specific
// company's trademark) containing a compass rose (echoing the Aviation
// page) and an orbiting satellite dot with a center star (echoing the
// ISS/Astro pages). All diagnostic readouts were previously here; they
// are intentionally hidden per request, not removed from the codebase
// logic (the NEXT/poll buttons still function via their existing touch
// coordinates in screen_manager_handle_touch, just without a visible
// button drawn here).
static void drawDebugBadge() {
  int cx = WIDTH / 2;
  int shieldTopCy = 210;
  int shieldR = 95;
  int apexY = 400;
  uint16_t badgeColor = screen.color565(200, 170, 90);
  uint16_t badgeAccent = screen.color565(230, 200, 120);

  float prevX = 0, prevY = 0;
  bool havePrev = false;
  for (int i = 0; i <= 60; i++) {
    float deg = 180.0f + 180.0f * (i / 60.0f);
    float rad = deg * PI / 180.0f;
    float px = cx + cosf(rad) * shieldR;
    float py = shieldTopCy + sinf(rad) * shieldR;
    if (havePrev) screen.drawLine((int)prevX, (int)prevY, (int)px, (int)py, badgeColor);
    prevX = px; prevY = py; havePrev = true;
  }
  int leftShoulderX = cx - shieldR, rightShoulderX = cx + shieldR;
  screen.drawLine(leftShoulderX, shieldTopCy, cx, apexY, badgeColor);
  screen.drawLine(rightShoulderX, shieldTopCy, cx, apexY, badgeColor);

  int compassCy = shieldTopCy + 40;
  int compassR = 70;
  screen.drawCircle(cx, compassCy, compassR, badgeColor);
  for (int deg = 0; deg < 360; deg += 30) {
    float rad = deg * PI / 180.0f;
    bool isCardinal = (deg % 90 == 0);
    int tickLen = isCardinal ? 10 : 5;
    int x0 = cx + (int)(sinf(rad) * compassR);
    int y0 = compassCy - (int)(cosf(rad) * compassR);
    int x1 = cx + (int)(sinf(rad) * (compassR - tickLen));
    int y1 = compassCy - (int)(cosf(rad) * (compassR - tickLen));
    screen.drawLine(x0, y0, x1, y1, isCardinal ? badgeAccent : badgeColor);
  }

  int orbitR = 42;
  screen.drawCircle(cx, compassCy, orbitR, badgeColor);
  uint32_t t = millis();
  float orbitAngle = (float)(t % 6000) / 6000.0f * 2.0f * PI;
  int satX = cx + (int)(cosf(orbitAngle) * orbitR);
  int satY = compassCy + (int)(sinf(orbitAngle) * orbitR);
  screen.fillCircle(satX, satY, 4, badgeAccent);

  int starOuterR = 20, starInnerR = 9;
  float starPtsX[10], starPtsY[10];
  for (int i = 0; i < 10; i++) {
    float ang = -PI / 2.0f + i * (PI / 5.0f);
    float r = (i % 2 == 0) ? (float)starOuterR : (float)starInnerR;
    starPtsX[i] = cx + cosf(ang) * r;
    starPtsY[i] = compassCy + sinf(ang) * r;
  }
  for (int i = 0; i < 10; i++) {
    int next = (i + 1) % 10;
    screen.fillTriangle(cx, compassCy, (int)starPtsX[i], (int)starPtsY[i],
                        (int)starPtsX[next], (int)starPtsY[next], badgeAccent);
  }
}

static void draw_debug() {
  drawDebugBadge();
}'''

content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Replaced Debug page diagnostics with an original decorative badge")

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
