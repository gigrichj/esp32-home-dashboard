#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/screens/screen_manager.cpp"
with open(path) as f:
    content = f.read()

old = '''  char heapLine[64];
  snprintf(heapLine, sizeof(heapLine), "Free heap: %u  Free PSRAM: %u",
           static_cast<unsigned>(ESP.getFreeHeap()),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString(heapLine, 10, 130);'''
assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"
new = old + (
    "\n\n"
    "  char largestBlockLine[64];\n"
    "  snprintf(largestBlockLine, sizeof(largestBlockLine), \"Largest free block (8BIT): %u\",\n"
    "           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));\n"
    "  screen.setTextSize(2);\n"
    "  screen.setTextColor(colorAccent, colorBg);\n"
    "  screen.drawString(largestBlockLine, 10, 160);"
)
content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Added largest-free-block (MALLOC_CAP_8BIT) readout to Debug tab")

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
