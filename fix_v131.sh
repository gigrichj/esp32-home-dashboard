#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/services/aviation_service.cpp"
with open(path) as f:
    content = f.read()

old = '''static void fetchAircraftType(const String& icaoHex) {
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
assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"

new = '''static void fetchAircraftType(const String& icaoHex) {
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
content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Added temporary debug logging to fetchAircraftType() for hex/URL/raw response/parsed values")

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
