#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/screens/screen_manager.cpp"
with open(path) as f:
    content = f.read()

old = '''  // A little airplane silhouette drifting across the lower part of the
  // shield, echoing the Dashboard's own background animation.
  {
    int span = shieldR * 2 - 20;
    int px = (cx - shieldR + 10) + (int)((t / 22) % (uint32_t)span);
    int py = apexY - 45;
    screen.fillRect(px - 14, py - 2, 20, 4, planeColor);
    screen.fillTriangle(px + 4, py - 3, px + 4, py + 3, px + 13, py, planeColor);
    screen.fillTriangle(px + 1, py, px - 7, py - 11, px - 2, py, planeColor);
    screen.fillTriangle(px + 1, py, px - 7, py + 11, px - 2, py, planeColor);
    screen.fillTriangle(px - 11, py, px - 16, py - 6, px - 13, py, planeColor);
    screen.fillTriangle(px - 11, py, px - 16, py + 6, px - 13, py, planeColor);
  }'''

assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"

new = '''  // A little airplane silhouette drifting across the FULL screen width
  // (not just the shield), echoing the Dashboard's own background
  // animation. Each pass picks a fresh random vertical position, so the
  // flight path varies across any portion of the screen instead of
  // repeating the same line every lap.
  {
    static const int HEADER_H = 40;
    static int planeY = -1;
    static int lastLap = -1;
    int span = WIDTH + 80;
    uint32_t cycle = t / 22;
    int x = (int)(cycle % (uint32_t)span) - 40;
    int lap = (int)(cycle / (uint32_t)span);
    if (lap != lastLap || planeY < 0) {
      lastLap = lap;
      planeY = HEADER_H + 40 + (int)(esp_random() % (uint32_t)(HEIGHT - HEADER_H - 80));
    }
    int px = x, py = planeY;
    screen.fillRect(px - 14, py - 2, 20, 4, planeColor);
    screen.fillTriangle(px + 4, py - 3, px + 4, py + 3, px + 13, py, planeColor);
    screen.fillTriangle(px + 1, py, px - 7, py - 11, px - 2, py, planeColor);
    screen.fillTriangle(px + 1, py, px - 7, py + 11, px - 2, py, planeColor);
    screen.fillTriangle(px - 11, py, px - 16, py - 6, px - 13, py, planeColor);
    screen.fillTriangle(px - 11, py, px - 16, py + 6, px - 13, py, planeColor);
  }'''

content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Plane in debug badge now drifts across the full screen with a random Y position each pass")

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
