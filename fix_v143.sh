#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/services/iss_service.cpp"
with open(path) as f:
    content = f.read()

old = '''static bool fetchAndInitTLE() {
  HTTPClient http;
  http.begin("https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[ISS] TLE fetch HTTP %d\\n", code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();'''
assert content.count(old) == 1, f"expected 1, found {content.count(old)}"

new = '''static bool fetchAndInitTLE() {
  HTTPClient http;
  http.begin("https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[ISS] TLE fetch HTTP %d\\n", code);
    http.end();
    return false;
  }

  // Read the body manually with an explicit timeout and a yield on every
  // iteration, instead of http.getString() -- the same fix applied to
  // aviation (v128) and astro (v133/v141) after connection resets were
  // found to interact badly with that call. Without a real TLE, the ground
  // track never draws (computeGroundTrack() short-circuits on !tleLoaded),
  // even though position/altitude (a separate API) can still work fine.
  int payloadLen = http.getSize();
  int bufSize = (payloadLen > 0) ? payloadLen + 1 : 8192;
  char *rawBuf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
  if (rawBuf == nullptr) {
    Serial.println("[ISS] TLE payload buffer alloc failed");
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t readTotal = 0;
  uint32_t startMs = millis();
  bool readError = false;
  while (readTotal < (size_t)(bufSize - 1) && millis() - startMs < 15000) {
    if (!http.connected() && stream->available() == 0) break;
    size_t avail = stream->available();
    if (avail > 0) {
      int toRead = (int)min(avail, (size_t)(bufSize - 1 - readTotal));
      int r = stream->readBytes(rawBuf + readTotal, toRead);
      if (r <= 0) { readError = true; break; }
      readTotal += r;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5)); // yield -- the critical fix
    }
  }
  rawBuf[readTotal] = '\\0';
  http.end();

  if (readError) {
    Serial.println("[ISS] TLE payload read error");
    free(rawBuf);
    return false;
  }

  String payload(rawBuf);
  free(rawBuf);'''
content = content.replace(old, new)

# Ensure required includes are present
if "#include <esp_heap_caps.h>" not in content:
    lines = content.split("\n")
    for i, line in enumerate(lines):
        if line.startswith("#include"):
            lines.insert(i + 1, "#include <esp_heap_caps.h>")
            break
    content = "\n".join(lines)
if "#include <WiFiClient.h>" not in content:
    lines = content.split("\n")
    for i, line in enumerate(lines):
        if line.startswith("#include"):
            lines.insert(i + 1, "#include <WiFiClient.h>")
            break
    content = "\n".join(lines)

with open(path, "w") as f:
    f.write(content)
print("Hardened fetchAndInitTLE() with the same safe manual read loop used for aviation/astro")

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
