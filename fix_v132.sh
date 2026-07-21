#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/services/aviation_service.cpp"
with open(path) as f:
    content = f.read()

old = '''void aviation_request_detail(const String& icaoHex, const String& callsign) {
  if (g_aircraftDetail.valid && g_aircraftDetail.lookedUpIcao == icaoHex) {
    return; // cache hit, already have this one
  }
  if (pendingDetailRequested && pendingIcao == icaoHex) {
    return; // already in flight
  }
  pendingIcao = icaoHex;
  pendingCallsign = callsign;
  pendingDetailRequested = true;
  g_aircraftDetail.valid = false;
  g_aircraftDetail.lookupInProgress = true;
  g_aircraftDetail.lookupError = "";
}'''
assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"

new = '''void aviation_request_detail(const String& icaoHex, const String& callsign) {
  Serial.printf("[Aviation] aviation_request_detail called: icaoHex='%s' callsign='%s' (cached valid=%d lookedUpIcao='%s', pending=%d pendingIcao='%s')\\n",
                icaoHex.c_str(), callsign.c_str(),
                g_aircraftDetail.valid, g_aircraftDetail.lookedUpIcao.c_str(),
                pendingDetailRequested, pendingIcao.c_str());
  if (g_aircraftDetail.valid && g_aircraftDetail.lookedUpIcao == icaoHex) {
    Serial.println("[Aviation] -> cache hit, skipping fetch");
    return; // cache hit, already have this one
  }
  if (pendingDetailRequested && pendingIcao == icaoHex) {
    Serial.println("[Aviation] -> already in flight, skipping");
    return; // already in flight
  }
  Serial.println("[Aviation] -> queuing new detail request");
  pendingIcao = icaoHex;
  pendingCallsign = callsign;
  pendingDetailRequested = true;
  g_aircraftDetail.valid = false;
  g_aircraftDetail.lookupInProgress = true;
  g_aircraftDetail.lookupError = "";
}'''
content = content.replace(old, new)

old2 = '''void aviation_service_detail_loop() {
  if (!pendingDetailRequested) return;
  if (!wifi_manager_is_connected()) return;

  String icaoHex = pendingIcao;
  String callsign = pendingCallsign;
  pendingDetailRequested = false;'''
assert content.count(old2) == 1, f"expected 1 occurrence, found {content.count(old2)}"
new2 = '''void aviation_service_detail_loop() {
  if (!pendingDetailRequested) return;
  if (!wifi_manager_is_connected()) {
    Serial.println("[Aviation] detail_loop: pending but WiFi not connected, skipping this cycle");
    return;
  }

  String icaoHex = pendingIcao;
  String callsign = pendingCallsign;
  pendingDetailRequested = false;
  Serial.printf("[Aviation] detail_loop: servicing pending request for icaoHex='%s' callsign='%s'\\n",
                icaoHex.c_str(), callsign.c_str());'''
content = content.replace(old2, new2)

with open(path, "w") as f:
    f.write(content)
print("Added logging at aviation_request_detail() call site and detail_loop entry")

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
