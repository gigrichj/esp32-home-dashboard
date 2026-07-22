#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/screens/screen_manager.cpp"
with open(path) as f:
    content = f.read()

# --- 1. Dashboard date/time: one size larger ---
old1 = '''  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.setTextDatum(textdatum_t::top_left);
  screen.drawString(formatCurrentDateTime(), 20, 55);'''
assert content.count(old1) == 1, f"expected 1, found {content.count(old1)}"
new1 = '''  screen.setTextSize(3);
  screen.setTextColor(colorAccent, colorBg);
  screen.setTextDatum(textdatum_t::top_left);
  screen.drawString(formatCurrentDateTime(), 20, 55);'''
content = content.replace(old1, new1)

# --- 2. Aviation: add nm label to the 2nd ring from the middle ---
old2 = '''  {
    float rad45 = 45.0f * PI / 180.0f;
    int rangeLabelX = RADAR_CX + (int)(sinf(rad45) * RADAR_RADIUS);
    int rangeLabelY = RADAR_CY - (int)(cosf(rad45) * RADAR_RADIUS);
    screen.setTextSize(1);
    screen.setTextColor(colorLabel, colorBg);
    screen.setTextDatum(textdatum_t::middle_center);
    char rangeLabel[16];
    snprintf(rangeLabel, sizeof(rangeLabel), "%.0fnm", RADAR_MAX_RANGE_NM);
    screen.drawString(rangeLabel, rangeLabelX, rangeLabelY);
  }'''
assert content.count(old2) == 1, f"expected 1, found {content.count(old2)}"
new2 = '''  {
    float rad45 = 45.0f * PI / 180.0f;
    int rangeLabelX = RADAR_CX + (int)(sinf(rad45) * RADAR_RADIUS);
    int rangeLabelY = RADAR_CY - (int)(cosf(rad45) * RADAR_RADIUS);
    screen.setTextSize(1);
    screen.setTextColor(colorLabel, colorBg);
    screen.setTextDatum(textdatum_t::middle_center);
    char rangeLabel[16];
    snprintf(rangeLabel, sizeof(rangeLabel), "%.0fnm", RADAR_MAX_RANGE_NM);
    screen.drawString(rangeLabel, rangeLabelX, rangeLabelY);

    // Same treatment for the 2nd ring from the middle (ring 2 of 4), so
    // the mid-range distance is labeled too, not just the outer edge.
    int ring2Radius = RADAR_RADIUS * 2 / RADAR_RINGS;
    int ring2LabelX = RADAR_CX + (int)(sinf(rad45) * ring2Radius);
    int ring2LabelY = RADAR_CY - (int)(cosf(rad45) * ring2Radius);
    float ring2Nm = RADAR_MAX_RANGE_NM * 2.0f / RADAR_RINGS;
    char ring2Label[16];
    snprintf(ring2Label, sizeof(ring2Label), "%.0fnm", ring2Nm);
    screen.drawString(ring2Label, ring2LabelX, ring2LabelY);
  }'''
content = content.replace(old2, new2)

# --- 3. ISS: add spacing between "Longitude" label and its value ---
old3 = '''  screen.drawString("Longitude", col1X, contentY + 32);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%.2f", g_iss.lon);
  screen.drawString(row, col1X + 110, contentY + 32);'''
assert content.count(old3) == 1, f"expected 1, found {content.count(old3)}"
new3 = '''  screen.drawString("Longitude", col1X, contentY + 32);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%.2f", g_iss.lon);
  screen.drawString(row, col1X + 130, contentY + 32);'''
content = content.replace(old3, new3)

# --- 4. Weather: center precip gauge + wind compass pair under the 5 AQI bars ---
old4 = '''    int gaugeCx = 575, gaugeCy = 228, gaugeR = 22;'''
assert content.count(old4) == 1, f"expected 1, found {content.count(old4)}"
new4 = '''    int gaugeCx = 560, gaugeCy = 228, gaugeR = 22; // shifted left 15px to center the precip+wind pair under the 5 AQI bars (which span x=520-720)'''
content = content.replace(old4, new4)

old5 = '''    int windCx = 695, windCy = 228, windR = 22;'''
assert content.count(old5) == 1, f"expected 1, found {content.count(old5)}"
new5 = '''    int windCx = 680, windCy = 228, windR = 22; // shifted left 15px, see gaugeCx comment above'''
content = content.replace(old5, new5)

with open(path, "w") as f:
    f.write(content)
print("Applied all 4 subtle layout changes")

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
