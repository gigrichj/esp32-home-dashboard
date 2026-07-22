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
  g_tleLastHttpCode = code;
  if (code != 200) {
    Serial.printf("[ISS] TLE fetch HTTP %d\\n", code);
    g_tleLastFailureReason = "HTTP " + String(code);
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
    g_tleLastFailureReason = "payload read error";
    free(rawBuf);
    return false;
  }

  String payload(rawBuf);
  free(rawBuf);

  int nl1 = payload.indexOf('\\n');
  int nl2 = payload.indexOf('\\n', nl1 + 1);
  if (nl1 < 0 || nl2 < 0) {
    Serial.println("[ISS] TLE response missing expected line breaks");
    g_tleLastFailureReason = "response missing line breaks (len=" + String(payload.length()) + ")";
    return false;
  }

  String nameLine = payload.substring(0, nl1);
  String line1 = payload.substring(nl1 + 1, nl2);
  String line2 = payload.substring(nl2 + 1);
  nameLine.trim();
  line1.trim();
  line2.trim();

  if (line1.length() < 60 || line2.length() < 60) {
    Serial.println("[ISS] TLE lines look truncated, skipping");
    g_tleLastFailureReason = "lines truncated (l1=" + String(line1.length()) + " l2=" + String(line2.length()) + ")";
    return false;
  }

  char nameBuf[25];
  char line1Buf[130];
  char line2Buf[130];
  strlcpy(nameBuf, nameLine.c_str(), sizeof(nameBuf));
  strlcpy(line1Buf, line1.c_str(), sizeof(line1Buf));
  strlcpy(line2Buf, line2.c_str(), sizeof(line2Buf));

  sat.site((double)HOME_LAT, (double)HOME_LON, 0);
  sat.init(nameBuf, line1Buf, line2Buf); // return value just means "TLE unchanged since last call" - not an error
  Serial.println("[ISS] TLE loaded/refreshed");
  g_tleLastFailureReason = "";
  return true;
}'''

assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"

new = '''// Reads an HTTP response body with an explicit timeout and a yield on
// every iteration, instead of http.getString() -- the same fix applied to
// aviation (v128) and astro (v133/v141) after connection resets were found
// to interact badly with that call. Shared by both TLE sources below.
static bool readTleBodySafely(HTTPClient& http, String& outPayload) {
  int payloadLen = http.getSize();
  int bufSize = (payloadLen > 0) ? payloadLen + 1 : 8192;
  char *rawBuf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
  if (rawBuf == nullptr) {
    Serial.println("[ISS] TLE payload buffer alloc failed");
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

  if (readError) {
    Serial.println("[ISS] TLE payload read error");
    free(rawBuf);
    return false;
  }

  outPayload = String(rawBuf);
  free(rawBuf);
  return true;
}

// Common finish step once we have a name + two TLE lines from either source.
static bool initSatFromTle(const String& nameLine, const String& line1, const String& line2) {
  if (line1.length() < 60 || line2.length() < 60) {
    Serial.println("[ISS] TLE lines look truncated, skipping");
    g_tleLastFailureReason = "lines truncated (l1=" + String(line1.length()) + " l2=" + String(line2.length()) + ")";
    return false;
  }

  char nameBuf[25];
  char line1Buf[130];
  char line2Buf[130];
  strlcpy(nameBuf, nameLine.c_str(), sizeof(nameBuf));
  strlcpy(line1Buf, line1.c_str(), sizeof(line1Buf));
  strlcpy(line2Buf, line2.c_str(), sizeof(line2Buf));

  sat.site((double)HOME_LAT, (double)HOME_LON, 0);
  sat.init(nameBuf, line1Buf, line2Buf); // return value just means "TLE unchanged since last call" - not an error
  Serial.println("[ISS] TLE loaded/refreshed");
  g_tleLastFailureReason = "";
  return true;
}

static bool fetchTLEFromCelestrak() {
  HTTPClient http;
  http.begin("https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE");
  int code = http.GET();
  g_tleLastHttpCode = code;
  if (code != 200) {
    Serial.printf("[ISS] Celestrak TLE fetch HTTP %d\\n", code);
    g_tleLastFailureReason = "Celestrak: HTTP " + String(code);
    http.end();
    return false;
  }

  String payload;
  if (!readTleBodySafely(http, payload)) {
    g_tleLastFailureReason = "Celestrak: payload read error";
    http.end();
    return false;
  }
  http.end();

  int nl1 = payload.indexOf('\\n');
  int nl2 = payload.indexOf('\\n', nl1 + 1);
  if (nl1 < 0 || nl2 < 0) {
    Serial.println("[ISS] Celestrak TLE response missing expected line breaks");
    g_tleLastFailureReason = "Celestrak: response missing line breaks (len=" + String(payload.length()) + ")";
    return false;
  }

  String nameLine = payload.substring(0, nl1);
  String line1 = payload.substring(nl1 + 1, nl2);
  String line2 = payload.substring(nl2 + 1);
  nameLine.trim();
  line1.trim();
  line2.trim();

  return initSatFromTle(nameLine, line1, line2);
}

// Fallback used only when Celestrak is unreachable. tle.ivanstanojevic.me
// (free, no auth) caches CelesTrak's own data daily and re-serves it as
// JSON from separate infrastructure -- a genuinely independent point of
// failure from Celestrak itself, so it stays up during a Celestrak outage
// like the one that prompted adding this (confirmed live via direct browser
// test on 2026-07-21).
static bool fetchTLEFromIvanstanojevic() {
  HTTPClient http;
  http.begin("https://tle.ivanstanojevic.me/api/tle/25544");
  int code = http.GET();
  g_tleLastHttpCode = code;
  if (code != 200) {
    Serial.printf("[ISS] tle.ivanstanojevic.me fetch HTTP %d\\n", code);
    g_tleLastFailureReason = "ivanstanojevic: HTTP " + String(code);
    http.end();
    return false;
  }

  String payload;
  if (!readTleBodySafely(http, payload)) {
    g_tleLastFailureReason = "ivanstanojevic: payload read error";
    http.end();
    return false;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[ISS] tle.ivanstanojevic.me JSON parse error: %s\\n", err.c_str());
    g_tleLastFailureReason = "ivanstanojevic: JSON parse: " + String(err.c_str());
    return false;
  }

  const char* name = doc["name"] | "ISS (ZARYA)";
  const char* line1 = doc["line1"] | "";
  const char* line2 = doc["line2"] | "";
  if (strlen(line1) == 0 || strlen(line2) == 0) {
    Serial.println("[ISS] tle.ivanstanojevic.me response missing line1/line2");
    g_tleLastFailureReason = "ivanstanojevic: response missing line1/line2";
    return false;
  }

  return initSatFromTle(String(name), String(line1), String(line2));
}

static bool fetchAndInitTLE() {
  if (fetchTLEFromCelestrak()) {
    return true;
  }

  Serial.println("[ISS] Celestrak failed, falling back to tle.ivanstanojevic.me");
  return fetchTLEFromIvanstanojevic();
}'''

content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Added tle.ivanstanojevic.me as an automatic fallback when Celestrak's TLE fetch fails")

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
