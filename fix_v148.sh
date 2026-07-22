#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/services/iss_service.cpp"
with open(path) as f:
    content = f.read()

old1 = '''static bool fetchTLEFromCelestrak() {
  HTTPClient http;
  http.begin("https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE");
  int code = http.GET();'''
assert content.count(old1) == 1, f"expected 1, found {content.count(old1)}"
new1 = '''static bool fetchTLEFromCelestrak() {
  HTTPClient http;
  http.begin("https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE");
  // Force HTTP/1.0 to avoid chunked transfer encoding -- our manual
  // raw-stream read loop doesn't strip chunk-size framing, which caused
  // "invalid input" JSON parse errors on Open-Meteo earlier tonight (v141)
  // even for small responses; applying the same fix here defensively.
  http.useHTTP10(true);
  int code = http.GET();'''
content = content.replace(old1, new1)

old2 = '''static bool fetchTLEFromIvanstanojevic() {
  HTTPClient http;
  http.begin("https://tle.ivanstanojevic.me/api/tle/25544");
  int code = http.GET();'''
assert content.count(old2) == 1, f"expected 1, found {content.count(old2)}"
new2 = '''static bool fetchTLEFromIvanstanojevic() {
  HTTPClient http;
  http.begin("https://tle.ivanstanojevic.me/api/tle/25544");
  // Force HTTP/1.0 to avoid chunked transfer encoding -- same fix as
  // Open-Meteo (v141); transfer encoding is a server choice independent
  // of response size, so even this small JSON response can arrive chunked
  // and get corrupted by our manual read loop, producing "invalid input".
  http.useHTTP10(true);
  int code = http.GET();'''
content = content.replace(old2, new2)

with open(path, "w") as f:
    f.write(content)
print("Forced HTTP/1.0 for both TLE fetch sources to avoid chunked transfer encoding")

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
