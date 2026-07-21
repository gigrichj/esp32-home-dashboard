#!/usr/bin/env bash
set -e

python3 << 'PYEOF'
path = "src/main.cpp"
with open(path) as f:
    content = f.read()

old = '''    screen_manager_draw();

    uint16_t dbgBg = screen.color565(0, 0, 0);
    uint16_t dbgText = touched ? screen.color565(80, 220, 100) : screen.color565(120, 120, 120);
    screen.fillRect(0, HEIGHT - 24, 240, 24, dbgBg);
    screen.setTextSize(1);
    screen.setTextColor(dbgText, dbgBg);
    screen.setTextDatum(textdatum_t::top_left);
    char touchDbg[64];
    if (!screen.touchAvailable()) {
      snprintf(touchDbg, sizeof(touchDbg), "TOUCH: controller NOT initialized");
    } else {
      snprintf(touchDbg, sizeof(touchDbg), "TOUCH: x=%d y=%d count=%d",
               touchX, touchY, screen.lastTouchReadCount());
    }
    screen.drawString(touchDbg, 6, HEIGHT - 18);

    if (!screen.present()) {'''
assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"

new = '''    screen_manager_draw();

    if (!screen.present()) {'''
content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Removed the touch debug overlay (TOUCH: x=/y=/count=) from every page")

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
PYEOF
