#!/usr/bin/env bash
set -e

python3 << 'EOF'
# --- Header: add TLE diagnostics ---
hpath = "src/services/iss_service.h"
with open(hpath) as f:
    hcontent = f.read()

old_h = '''static const int ISS_TRACK_POINTS = 60;
extern TrackPoint g_issTrack[ISS_TRACK_POINTS];
extern int g_issTrackCount;
extern bool g_issTrackValid;'''
assert hcontent.count(old_h) == 1, f"expected 1, found {hcontent.count(old_h)}"
new_h = '''static const int ISS_TRACK_POINTS = 60;
extern TrackPoint g_issTrack[ISS_TRACK_POINTS];
extern int g_issTrackCount;
extern bool g_issTrackValid;

// Last HTTP result from the TLE (orbital elements) fetch, and a human-
// readable reason if it failed -- the ground track can't draw without a
// successfully loaded TLE, so this gives on-screen visibility into why,
// without needing a serial monitor.
extern int g_tleLastHttpCode;
extern String g_tleLastFailureReason;'''
hcontent = hcontent.replace(old_h, new_h)

with open(hpath, "w") as f:
    f.write(hcontent)
print("Added g_tleLastHttpCode/g_tleLastFailureReason to iss_service.h")

# --- .cpp: populate them ---
path = "src/services/iss_service.cpp"
with open(path) as f:
    content = f.read()

old1 = '''static bool tleLoaded = false;'''
assert content.count(old1) == 1, f"expected 1, found {content.count(old1)}"
new1 = '''static bool tleLoaded = false;
int g_tleLastHttpCode = -999;
String g_tleLastFailureReason = "";'''
content = content.replace(old1, new1)

old2 = '''  int code = http.GET();
  if (code != 200) {
    Serial.printf("[ISS] TLE fetch HTTP %d\\n", code);
    http.end();
    return false;
  }'''
assert content.count(old2) == 1, f"expected 1, found {content.count(old2)}"
new2 = '''  int code = http.GET();
  g_tleLastHttpCode = code;
  if (code != 200) {
    Serial.printf("[ISS] TLE fetch HTTP %d\\n", code);
    g_tleLastFailureReason = "HTTP " + String(code);
    http.end();
    return false;
  }'''
content = content.replace(old2, new2)

old3 = '''  if (readError) {
    Serial.println("[ISS] TLE payload read error");
    free(rawBuf);
    return false;
  }'''
assert content.count(old3) == 1, f"expected 1, found {content.count(old3)}"
new3 = '''  if (readError) {
    Serial.println("[ISS] TLE payload read error");
    g_tleLastFailureReason = "payload read error";
    free(rawBuf);
    return false;
  }'''
content = content.replace(old3, new3)

old4 = '''  if (nl1 < 0 || nl2 < 0) {
    Serial.println("[ISS] TLE response missing expected line breaks");
    return false;
  }'''
assert content.count(old4) == 1, f"expected 1, found {content.count(old4)}"
new4 = '''  if (nl1 < 0 || nl2 < 0) {
    Serial.println("[ISS] TLE response missing expected line breaks");
    g_tleLastFailureReason = "response missing line breaks (len=" + String(payload.length()) + ")";
    return false;
  }'''
content = content.replace(old4, new4)

old5 = '''  if (line1.length() < 60 || line2.length() < 60) {
    Serial.println("[ISS] TLE lines look truncated, skipping");
    return false;
  }'''
assert content.count(old5) == 1, f"expected 1, found {content.count(old5)}"
new5 = '''  if (line1.length() < 60 || line2.length() < 60) {
    Serial.println("[ISS] TLE lines look truncated, skipping");
    g_tleLastFailureReason = "lines truncated (l1=" + String(line1.length()) + " l2=" + String(line2.length()) + ")";
    return false;
  }'''
content = content.replace(old5, new5)

old6 = '''  sat.init(nameBuf, line1Buf, line2Buf); // return value just means "TLE unchanged since last call" - not an error
  Serial.println("[ISS] TLE loaded/refreshed");
  return true;
}'''
assert content.count(old6) == 1, f"expected 1, found {content.count(old6)}"
new6 = '''  sat.init(nameBuf, line1Buf, line2Buf); // return value just means "TLE unchanged since last call" - not an error
  Serial.println("[ISS] TLE loaded/refreshed");
  g_tleLastFailureReason = "";
  return true;
}'''
content = content.replace(old6, new6)

with open(path, "w") as f:
    f.write(content)
print("Wired g_tleLastHttpCode/g_tleLastFailureReason into every failure point of fetchAndInitTLE()")

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
