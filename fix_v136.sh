#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/services/astro_seeing_service.cpp"
with open(path) as f:
    content = f.read()

old = '''void astro_seeing_service_update() {
  astro_recompute_moon_phase();

  if (!wifi_manager_is_connected()) return;

  if (fetch7Timer()) {'''
assert content.count(old) == 1, f"expected 1, found {content.count(old)}"

new = '''void astro_seeing_service_update() {
  astro_recompute_moon_phase();

  Serial.printf("[Astro] astro_seeing_service_update called, wifi connected=%d\\n", wifi_manager_is_connected());

  if (!wifi_manager_is_connected()) {
    Serial.println("[Astro] WiFi not connected, skipping this cycle");
    return;
  }

  if (fetch7Timer()) {'''
content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Added entry logging to astro_seeing_service_update() to check WiFi connectivity at call time")

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
