#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/screens/screen_manager.cpp"
with open(path) as f:
    content = f.read()

old = '''static void drawDebugBadge() {
  int cx = WIDTH / 2;
  int shieldR = 140;      // scaled up (was 95) so the badge fills the screen top-to-bottom
  int shieldTopCy = 20 + shieldR;  // top of the rounded arc sits ~20px from the screen's top edge
  int apexY = HEIGHT - 20;         // point of the shield sits ~20px from the screen's bottom edge
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

  int compassCy = shieldTopCy + 59;  // same proportion as before, scaled to the larger shield
  int compassR = 103;
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

  int orbitR = 62;
  screen.drawCircle(cx, compassCy, orbitR, badgeColor);
  uint32_t t = millis();
  float orbitAngle = (float)(t % 6000) / 6000.0f * 2.0f * PI;
  int satX = cx + (int)(cosf(orbitAngle) * orbitR);
  int satY = compassCy + (int)(sinf(orbitAngle) * orbitR);
  screen.fillCircle(satX, satY, 4, badgeAccent);

  int starOuterR = 29, starInnerR = 13;
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
}'''

assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"

new = '''// Draws line segments 3 times, offset by 1px along the perpendicular of
// the segment's direction, to simulate a thicker stroke -- this display
// library has no line-width parameter.
static void drawThickLine(int x0, int y0, int x1, int y1, uint16_t color) {
  screen.drawLine(x0, y0, x1, y1, color);
  float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.001f) return;
  int px = (int)roundf(-dy / len);
  int py = (int)roundf(dx / len);
  screen.drawLine(x0 + px, y0 + py, x1 + px, y1 + py, color);
  screen.drawLine(x0 - px, y0 - py, x1 - px, y1 - py, color);
}

static void drawThickCircle(int cx, int cy, int r, uint16_t color) {
  screen.drawCircle(cx, cy, r, color);
  screen.drawCircle(cx, cy, r - 1, color);
  screen.drawCircle(cx, cy, r + 1, color);
}

static void drawDebugBadge() {
  int cx = WIDTH / 2;
  static const int HEADER_H = 40;
  int shieldR = 130;                    // slightly smaller than before to fit below the header
  int shieldTopCy = HEADER_H + 10 + shieldR;  // top of the arc sits just below the header band
  int apexY = HEIGHT - 20;

  uint16_t shieldColor = screen.color565(190, 60, 60);     // deep red outline
  uint16_t compassColor = screen.color565(190, 195, 205);  // silver
  uint16_t compassAccent = screen.color565(230, 230, 240); // bright silver for cardinal ticks
  uint16_t starColor = screen.color565(235, 195, 70);      // gold
  uint16_t issColor = screen.color565(150, 170, 235);      // pale blue
  uint16_t planeColor = screen.color565(120, 200, 150);    // soft green
  uint16_t galaxyColor = screen.color565(170, 120, 210);   // violet
  uint16_t galaxyCoreColor = screen.color565(230, 210, 255);

  // Shield outline, thicker stroke.
  float prevX = 0, prevY = 0;
  bool havePrev = false;
  for (int i = 0; i <= 60; i++) {
    float deg = 180.0f + 180.0f * (i / 60.0f);
    float rad = deg * PI / 180.0f;
    float px = cx + cosf(rad) * shieldR;
    float py = shieldTopCy + sinf(rad) * shieldR;
    if (havePrev) drawThickLine((int)prevX, (int)prevY, (int)px, (int)py, shieldColor);
    prevX = px; prevY = py; havePrev = true;
  }
  int leftShoulderX = cx - shieldR, rightShoulderX = cx + shieldR;
  drawThickLine(leftShoulderX, shieldTopCy, cx, apexY, shieldColor);
  drawThickLine(rightShoulderX, shieldTopCy, cx, apexY, shieldColor);

  // Compass rose, thicker ring.
  int compassCy = shieldTopCy + 55;
  int compassR = 96;
  drawThickCircle(cx, compassCy, compassR, compassColor);
  for (int deg = 0; deg < 360; deg += 30) {
    float rad = deg * PI / 180.0f;
    bool isCardinal = (deg % 90 == 0);
    int tickLen = isCardinal ? 10 : 5;
    int x0 = cx + (int)(sinf(rad) * compassR);
    int y0 = compassCy - (int)(cosf(rad) * compassR);
    int x1 = cx + (int)(sinf(rad) * (compassR - tickLen));
    int y1 = compassCy - (int)(cosf(rad) * (compassR - tickLen));
    drawThickLine(x0, y0, x1, y1, isCardinal ? compassAccent : compassColor);
  }

  // Small rotating galaxy -- a spiral of dots around a bright core -- tucked
  // in the upper part of the shield, above the compass rose.
  {
    int galaxyCx = cx, galaxyCy = shieldTopCy - shieldR / 2 + 8;
    uint32_t t = millis();
    float baseAngle = (float)(t % 9000) / 9000.0f * 2.0f * PI;
    for (int i = 0; i < 10; i++) {
      float armAngle = baseAngle + (float)i * (2.0f * PI / 10.0f);
      float armR = 6.0f + (float)i * 1.8f;
      int dx = galaxyCx + (int)(cosf(armAngle) * armR);
      int dy = galaxyCy + (int)(sinf(armAngle) * armR * 0.6f); // flattened, spiral-galaxy look
      screen.fillCircle(dx, dy, 2, galaxyColor);
    }
    screen.fillCircle(galaxyCx, galaxyCy, 4, galaxyCoreColor);
  }

  // Orbit ring with an ISS-style icon (body + solar panel arms, not just a
  // dot) drifting around it.
  int orbitR = 58;
  drawThickCircle(cx, compassCy, orbitR, compassColor);
  uint32_t t = millis();
  float orbitAngle = (float)(t % 6000) / 6000.0f * 2.0f * PI;
  int satX = cx + (int)(cosf(orbitAngle) * orbitR);
  int satY = compassCy + (int)(sinf(orbitAngle) * orbitR);
  screen.fillRect(satX - 3, satY - 3, 6, 6, issColor);
  screen.fillRect(satX - 12, satY - 2, 8, 4, issColor);
  screen.fillRect(satX + 4, satY - 2, 8, 4, issColor);

  // A little airplane silhouette drifting across the lower part of the
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
  }

  // Center star, gold.
  int starOuterR = 27, starInnerR = 12;
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
                        (int)starPtsX[next], (int)starPtsY[next], starColor);
  }
}'''

content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Enhanced debug badge: moved below header, added plane/ISS/galaxy motifs, thicker strokes, richer color palette")

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
