#!/usr/bin/env bash
set -e

python3 << 'EOF'
# --- Patch 1: guard aviation states payload size before parsing ---
path = "src/services/aviation_service.cpp"
with open(path) as f:
    content = f.read()

old = (
    "  g_aviationStatus.lastHttpCode = code;\n"
    "\n"
    "  if (code == 200) {\n"
    "    String payload = http.getString();\n"
)
assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"
new = (
    "  g_aviationStatus.lastHttpCode = code;\n"
    "\n"
    "  if (code == 200) {\n"
    "    int payloadLen = http.getSize();\n"
    "    // Guard against an implausibly large response -- the lamin/lomin/lamax/lomax\n"
    "    // bounding box should keep this to a small local dataset. If OpenSky ever\n"
    "    // ignores the bounding box (rate limiting, auth hiccup, API change) and\n"
    "    // returns the full global states dataset instead, it can be several MB;\n"
    "    // letting http.getString() try to allocate that is suspected of causing\n"
    "    // the standalone heap-exhaustion crashes seen with an empty aircraft list.\n"
    "    if (payloadLen > 100000) {\n"
    "      Serial.printf(\"[Aviation] states payload implausibly large (%d bytes), skipping\\n\", payloadLen);\n"
    "      g_aviationStatus.lastError = \"States payload too large, skipped\";\n"
    "      http.end();\n"
    "      return;\n"
    "    }\n"
    "    String payload = http.getString();\n"
)
content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Patched src/services/aviation_service.cpp")

# --- Patch 2: add esp_heap_caps.h include to screen_manager.cpp if missing ---
sm_path = "src/screens/screen_manager.cpp"
with open(sm_path) as f:
    sm = f.read()

if "#include <esp_heap_caps.h>" not in sm:
    lines = sm.split("\n")
    inserted = False
    for i, line in enumerate(lines):
        if line.startswith("#include"):
            lines.insert(i + 1, "#include <esp_heap_caps.h>")
            inserted = True
            break
    assert inserted, "could not find any #include line to anchor on"
    sm = "\n".join(lines)
    print("Added #include <esp_heap_caps.h> to screen_manager.cpp")
else:
    print("esp_heap_caps.h already included, skipping")

# --- Patch 3: add live free-heap line to Debug tab ---
old_debug = (
    '  screen.fillRect(230, 310, 180, 60, colorAccent);\n'
    '  screen.setTextColor(colorBg, colorAccent);\n'
    '  screen.setTextDatum(textdatum_t::middle_center);\n'
    '  screen.drawString("NEXT >>", 320, 340);\n'
    '  screen.setTextDatum(textdatum_t::top_left);\n'
)
assert sm.count(old_debug) == 1, f"expected 1 occurrence, found {sm.count(old_debug)}"
new_debug = old_debug + (
    "\n"
    "  char heapLine[64];\n"
    "  snprintf(heapLine, sizeof(heapLine), \"Free heap: %u  Free PSRAM: %u\",\n"
    "           static_cast<unsigned>(ESP.getFreeHeap()),\n"
    "           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));\n"
    "  screen.setTextSize(2);\n"
    "  screen.setTextColor(colorAccent, colorBg);\n"
    "  screen.drawString(heapLine, 10, 130);\n"
)
sm = sm.replace(old_debug, new_debug)
with open(sm_path, "w") as f:
    f.write(sm)
print("Added live heap readout to draw_debug()")

# --- Patch 4: bump version ---
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
