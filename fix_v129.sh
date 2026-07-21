#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/services/mqtt_service.cpp"
with open(path) as f:
    content = f.read()

old = '''void mqtt_service_loop() {
  if (!mqttClient.connected()) {
    if (mqttClient.connect("esp32-dashboard", MQTT_USER, MQTT_PASSWORD)) {
      mqttClient.subscribe("dashboard/notify");
      mqttClient.subscribe("home/#"); // house-wide status/events, filter in onMessage
    }
    return;
  }
  mqttClient.loop();
}'''
assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"

new = '''// If the broker is unreachable/unconfigured, mqttClient.connect() was being
// retried every single networkTask iteration (~10-30ms) with zero backoff --
// hammering the network stack with rapid-fire connection resets whenever the
// broker refused or reset the socket. Now retries are spaced at least 5s
// apart, so a dead/misconfigured broker can no longer spin like that.
static uint32_t lastMqttAttemptMs = 0;
static const uint32_t MQTT_RETRY_INTERVAL_MS = 5000;

void mqtt_service_loop() {
  if (!mqttClient.connected()) {
    uint32_t now = millis();
    if (now - lastMqttAttemptMs < MQTT_RETRY_INTERVAL_MS) {
      return;
    }
    lastMqttAttemptMs = now;
    if (mqttClient.connect("esp32-dashboard", MQTT_USER, MQTT_PASSWORD)) {
      mqttClient.subscribe("dashboard/notify");
      mqttClient.subscribe("home/#"); // house-wide status/events, filter in onMessage
    }
    return;
  }
  mqttClient.loop();
}'''
content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Added 5s backoff to MQTT reconnect attempts")

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
