#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/services/astro_seeing_service.cpp"
with open(path) as f:
    content = f.read()

old = '''  http.begin(url);
  http.setTimeout(15000);
  int code = http.GET();
  // Expose this fallback's own HTTP code on the Debug tab'''
assert content.count(old) == 1, f"expected 1, found {content.count(old)}"

new = '''  http.begin(url);
  http.setTimeout(15000);
  // Force HTTP/1.0 -- Open-Meteo's larger response was suspected to arrive
  // chunked (Transfer-Encoding: chunked, no Content-Length), and our manual
  // raw-stream read loop (readHttpBodySafely, used since v128) doesn't
  // strip chunk-size framing the way http.getString() would -- resulting
  // in "invalid input" JSON parse errors even though the underlying data
  // was valid. HTTP/1.0 has no chunked transfer mechanism, so servers
  // typically respond with a plain Content-Length body instead.
  http.useHTTP10(true);
  int code = http.GET();
  // Expose this fallback's own HTTP code on the Debug tab'''
content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Forced HTTP/1.0 for the Open-Meteo request to avoid chunked transfer encoding")

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
