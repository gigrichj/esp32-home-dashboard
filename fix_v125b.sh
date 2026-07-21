#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/screens/screen_manager.cpp"
with open(path) as f:
    content = f.read()

# --- Call site 1: Sunrise/Sunset rows on Weather tab ---
old1 = '''  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Sunrise", 20, y);
  screen.setTextColor(colorText, colorBg);
  screen.drawString(formatHHMM(g_weather.sunriseUnix), 260, y);
  y += 30;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Sunset", 20, y);
  screen.setTextColor(colorText, colorBg);
  screen.drawString(formatHHMM(g_weather.sunsetUnix), 260, y);'''
assert content.count(old1) == 1, f"call site 1: expected 1, found {content.count(old1)}"
new1 = '''  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Sunrise", 20, y);
  screen.setTextColor(colorText, colorBg);
  {
    char sunriseBuf[16];
    formatHHMM(g_weather.sunriseUnix, sunriseBuf, sizeof(sunriseBuf));
    screen.drawString(sunriseBuf, 260, y);
  }
  y += 30;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Sunset", 20, y);
  screen.setTextColor(colorText, colorBg);
  {
    char sunsetBuf[16];
    formatHHMM(g_weather.sunsetUnix, sunsetBuf, sizeof(sunsetBuf));
    screen.drawString(sunsetBuf, 260, y);
  }'''
content = content.replace(old1, new1)

# --- Call site 2: ISS passes list ---
old2 = '''        char line[64];
        snprintf(line, sizeof(line), "%-6s%-7sEl%-3d",
                 formatPassDate(p.startUnix).c_str(),
                 formatPassTime(p.startUnix).c_str(),
                 p.maxElevationDeg);'''
assert content.count(old2) == 1, f"call site 2: expected 1, found {content.count(old2)}"
new2 = '''        char line[64];
        char passDateBuf[16];
        char passTimeBuf[16];
        formatPassDate(p.startUnix, passDateBuf, sizeof(passDateBuf));
        formatPassTime(p.startUnix, passTimeBuf, sizeof(passTimeBuf));
        snprintf(line, sizeof(line), "%-6s%-7sEl%-3d",
                 passDateBuf,
                 passTimeBuf,
                 p.maxElevationDeg);'''
content = content.replace(old2, new2)

# --- Call site 3: Debug tab sunrise/sunset readout ---
old3 = '''    char srLine[64];
    char ssLine[64];
    snprintf(srLine, sizeof(srLine), "SUNRISE %s UNIX %lu",
             formatHHMM(g_weather.sunriseUnix).c_str(),
             (unsigned long)g_weather.sunriseUnix);
    snprintf(ssLine, sizeof(ssLine), "SUNSET %s UNIX %lu",
             formatHHMM(g_weather.sunsetUnix).c_str(),
             (unsigned long)g_weather.sunsetUnix);'''
assert content.count(old3) == 1, f"call site 3: expected 1, found {content.count(old3)}"
new3 = '''    char srLine[64];
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
             (unsigned long)g_weather.sunsetUnix);'''
content = content.replace(old3, new3)

# --- Call site 4: Astro "Best night" summary ---
old4 = '''        char bestLine[32];
        snprintf(bestLine, sizeof(bestLine), "Best: %s %s",
                 formatPassDate(g_astroForecast[bestIdx].unixTime).c_str(),
                 formatPassTime(g_astroForecast[bestIdx].unixTime).c_str());'''
assert content.count(old4) == 1, f"call site 4: expected 1, found {content.count(old4)}"
new4 = '''        char bestLine[32];
        char bestDateBuf[16];
        char bestTimeBuf[16];
        formatPassDate(g_astroForecast[bestIdx].unixTime, bestDateBuf, sizeof(bestDateBuf));
        formatPassTime(g_astroForecast[bestIdx].unixTime, bestTimeBuf, sizeof(bestTimeBuf));
        snprintf(bestLine, sizeof(bestLine), "Best: %s %s",
                 bestDateBuf,
                 bestTimeBuf);'''
content = content.replace(old4, new4)

with open(path, "w") as f:
    f.write(content)
print("Updated all 4 call sites to use caller-supplied buffers")

# --- bump version ---
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
