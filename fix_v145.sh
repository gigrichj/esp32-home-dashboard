#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/screens/screen_manager.cpp"
with open(path) as f:
    content = f.read()

old = '''  if (g_issTrackValid && g_issTrackCount > 1) {
    uint16_t colorTrack = screen.color565(255, 170, 60);
    int prevX = 0, prevY = 0;
    bool havePrev = false;
    for (int i = 0; i < g_issTrackCount; i++) {
      int px = MAP_X + (int)((g_issTrack[i].lon + 180) / 360.0f * MAP_W);
      int py = MAP_Y + (int)((90 - g_issTrack[i].lat) / 180.0f * MAP_H);
      if (havePrev && abs(px - prevX) < MAP_W / 2) {
        screen.drawLine(prevX, prevY, px, py, colorTrack);
      }
      prevX = px;
      prevY = py;
      havePrev = true;
    }'''
assert content.count(old) == 1, f"expected 1, found {content.count(old)}"

new = '''  if (g_issTrackValid && g_issTrackCount > 1) {
    uint16_t colorTrack = screen.color565(255, 170, 60);
    int prevX = 0, prevY = 0;
    bool havePrev = false;
    for (int i = 0; i < g_issTrackCount; i++) {
      int px = MAP_X + (int)((g_issTrack[i].lon + 180) / 360.0f * MAP_W);
      int py = MAP_Y + (int)((90 - g_issTrack[i].lat) / 180.0f * MAP_H);
      if (havePrev && abs(px - prevX) < MAP_W / 2) {
        screen.drawLine(prevX, prevY, px, py, colorTrack);
      }
      prevX = px;
      prevY = py;
      havePrev = true;
    }
  } else {
    // No ground track without a successfully loaded TLE -- show why on
    // screen, since serial logging hasn't been reliably captured tonight.
    char tleLine[64];
    snprintf(tleLine, sizeof(tleLine), "TLE HTTP: %d", g_tleLastHttpCode);
    screen.setTextSize(1);
    screen.setTextColor(colorDim, colorBg);
    screen.setTextDatum(textdatum_t::top_left);
    screen.drawString(tleLine, MAP_X + 4, MAP_Y + 4);
    if (g_tleLastFailureReason.length() > 0) {
      screen.drawString(g_tleLastFailureReason, MAP_X + 4, MAP_Y + 18);
    }'''
content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Added on-screen TLE HTTP code + failure reason when ground track is missing")

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
