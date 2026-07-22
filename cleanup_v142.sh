#!/usr/bin/env bash
set -e

python3 << 'EOF'
# --- aviation_service.cpp: remove v131/v132 photo-debug tracing ---
path = "src/services/aviation_service.cpp"
with open(path) as f:
    content = f.read()

old1 = '''void aviation_request_detail(const String& icaoHex, const String& callsign) {
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
  pendingIcao = icaoHex;'''
assert content.count(old1) == 1, f"expected 1, found {content.count(old1)}"
new1 = '''void aviation_request_detail(const String& icaoHex, const String& callsign) {
  if (g_aircraftDetail.valid && g_aircraftDetail.lookedUpIcao == icaoHex) {
    return; // cache hit, already have this one
  }
  if (pendingDetailRequested && pendingIcao == icaoHex) {
    return; // already in flight
  }
  pendingIcao = icaoHex;'''
content = content.replace(old1, new1)

old2 = '''static void fetchAircraftType(const String& icaoHex) {
  HTTPClient http;
  String url = "https://api.adsbdb.com/v0/aircraft/" + icaoHex;
  Serial.printf("[Aviation] adsbdb aircraft lookup URL: %s\\n", url.c_str());
  http.begin(url);
  int code = http.GET();
  Serial.printf("[Aviation] adsbdb aircraft lookup HTTP %d\\n", code);
  if (code == 200) {
    String payload = http.getString();
    Serial.printf("[Aviation] adsbdb aircraft raw response: %s\\n", payload.c_str());
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      JsonObject aircraft = doc["response"]["aircraft"];
      if (!aircraft.isNull()) {
        g_aircraftDetail.type = aircraft["type"].as<String>();
        const char* thumb = aircraft["url_photo_thumbnail"];
        g_aircraftDetail.photoThumbUrl = thumb ? String(thumb) : String("");
        Serial.printf("[Aviation] parsed type='%s' photoThumbUrl='%s'\\n",
                      g_aircraftDetail.type.c_str(), g_aircraftDetail.photoThumbUrl.c_str());
      } else {
        Serial.println("[Aviation] adsbdb response['response']['aircraft'] is null/missing");
      }
    } else {
      Serial.printf("[Aviation] adsbdb aircraft JSON parse error: %s\\n", err.c_str());
    }
  }
  http.end();
}'''
assert content.count(old2) == 1, f"expected 1, found {content.count(old2)}"
new2 = '''static void fetchAircraftType(const String& icaoHex) {
  HTTPClient http;
  String url = "https://api.adsbdb.com/v0/aircraft/" + icaoHex;
  http.begin(url);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      JsonObject aircraft = doc["response"]["aircraft"];
      if (!aircraft.isNull()) {
        g_aircraftDetail.type = aircraft["type"].as<String>();
        const char* thumb = aircraft["url_photo_thumbnail"];
        g_aircraftDetail.photoThumbUrl = thumb ? String(thumb) : String("");
      }
    }
  } else {
    Serial.printf("[Aviation] adsbdb aircraft lookup HTTP %d\\n", code);
  }
  http.end();
}'''
content = content.replace(old2, new2)

old3 = '''void aviation_service_detail_loop() {
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
assert content.count(old3) == 1, f"expected 1, found {content.count(old3)}"
new3 = '''void aviation_service_detail_loop() {
  if (!pendingDetailRequested) return;
  if (!wifi_manager_is_connected()) return;

  String icaoHex = pendingIcao;
  String callsign = pendingCallsign;
  pendingDetailRequested = false;'''
content = content.replace(old3, new3)

with open(path, "w") as f:
    f.write(content)
print("Cleaned up aviation_service.cpp: removed v131/v132 photo-debug tracing")

# --- astro_seeing_service.cpp: remove v136 entry logging ---
path2 = "src/services/astro_seeing_service.cpp"
with open(path2) as f:
    content2 = f.read()

old4 = '''void astro_seeing_service_update() {
  astro_recompute_moon_phase();

  Serial.printf("[Astro] astro_seeing_service_update called, wifi connected=%d\\n", wifi_manager_is_connected());

  if (!wifi_manager_is_connected()) {
    Serial.println("[Astro] WiFi not connected, skipping this cycle");
    return;
  }

  if (fetch7Timer()) {'''
assert content2.count(old4) == 1, f"expected 1, found {content2.count(old4)}"
new4 = '''void astro_seeing_service_update() {
  astro_recompute_moon_phase();

  if (!wifi_manager_is_connected()) return;

  if (fetch7Timer()) {'''
content2 = content2.replace(old4, new4)

with open(path2, "w") as f:
    f.write(content2)
print("Cleaned up astro_seeing_service.cpp: removed v136 entry logging")

# --- main.cpp: remove v137 heartbeat + mqtt timing ---
path3 = "src/main.cpp"
with open(path3) as f:
    content3 = f.read()

old5 = '''    uint32_t mqttStartMs = millis();
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
    }
'''
assert content3.count(old5) == 1, f"expected 1, found {content3.count(old5)}"
new5 = '''    mqtt_service_loop();

    uint32_t now = millis();
'''
content3 = content3.replace(old5, new5)

with open(path3, "w") as f:
    f.write(content3)
print("Cleaned up main.cpp: removed v137 networkTask heartbeat + mqtt timing")

# --- wifi_manager.cpp: remove v138 handleClient timing ---
path4 = "src/services/wifi_manager.cpp"
with open(path4) as f:
    content4 = f.read()

old6 = '''  uint32_t handleClientStartMs = millis();
  server.handleClient();
  uint32_t handleClientDurationMs = millis() - handleClientStartMs;
  if (handleClientDurationMs > 100) {
    Serial.printf("[WiFi] server.handleClient() took %lums this cycle\\n", (unsigned long)handleClientDurationMs);
  }'''
assert content4.count(old6) == 1, f"expected 1, found {content4.count(old6)}"
new6 = '''  server.handleClient();'''
content4 = content4.replace(old6, new6)

with open(path4, "w") as f:
    f.write(content4)
print("Cleaned up wifi_manager.cpp: removed v138 handleClient timing")

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
