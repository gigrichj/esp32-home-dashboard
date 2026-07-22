#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/main.cpp"
with open(path) as f:
    content = f.read()

old = '''    mqtt_service_loop();

    uint32_t now = millis();'''
assert content.count(old) == 1, f"expected 1, found {content.count(old)}"

new = '''    uint32_t mqttStartMs = millis();
    mqtt_service_loop();
    uint32_t mqttDurationMs = millis() - mqttStartMs;
    if (mqttDurationMs > 100) {
      Serial.printf("[networkTask] mqtt_service_loop() took %lums this cycle\\n", (unsigned long)mqttDurationMs);
    }

    uint32_t now = millis();

    static uint32_t lastHeartbeatMs = 0;
    if (now - lastHeartbeatMs > 5000) {
      lastHeartbeatMs = now;
      uint32_t astroIntervalDbg = astroDataLoaded ? ASTRO_POLL_MS : ASTRO_RETRY_MS;
      Serial.printf("[networkTask] heartbeat now=%lu lastAstro=%lu diff=%lu astroInterval=%lu astroDataLoaded=%d\\n",
                    (unsigned long)now, (unsigned long)lastAstro, (unsigned long)(now - lastAstro),
                    (unsigned long)astroIntervalDbg, astroDataLoaded);
    }'''
content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Added networkTask heartbeat logging (every 5s) and mqtt_service_loop() timing check")

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
