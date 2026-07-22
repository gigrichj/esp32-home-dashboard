#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/screens/screen_manager.cpp"
with open(path) as f:
    content = f.read()

old = '''    int li = g_astroForecast[tonightIdx].liftedindex;
    screen.setTextSize(2);
    screen.setTextColor(li > 0 ? colorSuccess : colorDanger, colorBg);
    screen.drawString(astro_instability_label(li), col3X, row2Y + 34);'''
assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"

new = '''    int li = g_astroForecast[tonightIdx].liftedindex;
    screen.setTextSize(2);
    screen.setTextColor(li > 0 ? colorSuccess : colorDanger, colorBg);
    screen.drawString(astro_instability_label(li), col3X, row2Y + 34);

    // Tiny legend under the value: all 4 possible Storm Risk levels,
    // best to worst, each in its matching color -- same GOOD/FAIR/POOR/BAD
    // color scale used elsewhere on this page (astroSeverityColor).
    {
      screen.setTextSize(1);
      int legendY = row2Y + 56;
      int legendX = col3X;
      const char* labels[4] = {"Stable", "Slight Risk", "Moderate Risk", "High Risk"};
      uint16_t colors[4] = {
        colorSuccess,
        screen.color565(230, 200, 40),
        screen.color565(230, 130, 40),
        colorDanger
      };
      char legendLine[8];
      screen.setTextColor(colorDim, colorBg);
      screen.drawString("(", legendX, legendY);
      legendX += 6;
      for (int i = 0; i < 4; i++) {
        screen.setTextColor(colors[i], colorBg);
        screen.drawString(labels[i], legendX, legendY);
        legendX += strlen(labels[i]) * 6;
        if (i < 3) {
          screen.setTextColor(colorDim, colorBg);
          screen.drawString(",", legendX, legendY);
          legendX += 6;
        }
      }
      screen.setTextColor(colorDim, colorBg);
      screen.drawString(")", legendX, legendY);
      (void)legendLine; // unused, kept for potential future formatting
    }'''

content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Added Storm Risk legend (best to worst, color-coded) below the value")

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
