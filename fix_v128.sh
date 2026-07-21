#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/services/aviation_service.cpp"
with open(path) as f:
    content = f.read()

old = '''    if (payloadLen > 100000) {
      Serial.printf("[Aviation] states payload implausibly large (%d bytes), skipping\\n", payloadLen);
      g_aviationStatus.lastError = "States payload too large, skipped";
      http.end();
      return;
    }
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);'''
assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"

new = '''    if (payloadLen > 100000) {
      Serial.printf("[Aviation] states payload implausibly large (%d bytes), skipping\\n", payloadLen);
      g_aviationStatus.lastError = "States payload too large, skipped";
      http.end();
      return;
    }

    // Read the body manually with an explicit timeout and a yield on every
    // iteration, instead of http.getString() -- that call (via
    // Stream::readString()) reads one byte at a time in a tight loop with no
    // yield, and if the peer resets the connection mid-read (observed:
    // errno 104 "Connection reset by peer"), the underlying mbedtls_ssl_read
    // can block long enough to starve the CPU 0 idle task and trip the
    // FreeRTOS task watchdog, crashing the whole device. This mirrors the
    // safe streaming pattern already used in fetchAndDecodePhoto() above.
    int bufSize = (payloadLen > 0) ? payloadLen + 1 : 65536;
    char *rawBuf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
    if (rawBuf == nullptr) {
      Serial.println("[Aviation] states payload buffer alloc failed");
      g_aviationStatus.lastError = "Buffer alloc failed";
      http.end();
      return;
    }

    WiFiClient *stream = http.getStreamPtr();
    size_t readTotal = 0;
    uint32_t startMs = millis();
    bool readError = false;
    while (readTotal < (size_t)(bufSize - 1) && millis() - startMs < 8000) {
      if (!http.connected() && stream->available() == 0) break;
      size_t avail = stream->available();
      if (avail > 0) {
        int toRead = (int)min(avail, (size_t)(bufSize - 1 - readTotal));
        int r = stream->readBytes(rawBuf + readTotal, toRead);
        if (r <= 0) { readError = true; break; }
        readTotal += r;
      } else {
        vTaskDelay(pdMS_TO_TICKS(5)); // yield -- this is the critical fix
      }
    }
    rawBuf[readTotal] = '\\0';

    if (readError) {
      Serial.println("[Aviation] states payload read error");
      g_aviationStatus.lastError = "States payload read error";
      free(rawBuf);
      http.end();
      return;
    }

    String payload(rawBuf);
    free(rawBuf);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);'''

content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Replaced http.getString() with a yield-safe manual read loop in aviation_service_update()")

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
