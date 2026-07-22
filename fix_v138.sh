#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/services/wifi_manager.cpp"
with open(path) as f:
    content = f.read()

old = '''void wifi_manager_loop() {
  if (inApMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    return;
  }

  server.handleClient();'''
assert content.count(old) == 1, f"expected 1, found {content.count(old)}"

new = '''void wifi_manager_loop() {
  if (inApMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    return;
  }

  uint32_t handleClientStartMs = millis();
  server.handleClient();
  uint32_t handleClientDurationMs = millis() - handleClientStartMs;
  if (handleClientDurationMs > 100) {
    Serial.printf("[WiFi] server.handleClient() took %lums this cycle\\n", (unsigned long)handleClientDurationMs);
  }'''
content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Added timing check around server.handleClient() to confirm/rule out blocking")

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
