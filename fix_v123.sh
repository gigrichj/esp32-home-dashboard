#!/usr/bin/env bash
set -e

python3 << 'EOF'
import re

path = "src/main.cpp"
with open(path) as f:
    content = f.read()

old = "    aviation_service_detail_loop();\n"
assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"
new = (
    "    if (!heavyFetchThisCycle) {\n"
    "      aviation_service_detail_loop();\n"
    "    }\n"
)
content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)

print("Patched src/main.cpp")

vpath = "src/version.h"
with open(vpath) as f:
    vcontent = f.read()

old_ver_pattern = re.search(r'#define FIRMWARE_VERSION "v(\d+)"', vcontent)
assert old_ver_pattern, "could not find FIRMWARE_VERSION define"
old_ver_line = old_ver_pattern.group(0)
new_ver_num = int(old_ver_pattern.group(1)) + 1
new_ver_line = f'#define FIRMWARE_VERSION "v{new_ver_num}"'

assert vcontent.count(old_ver_line) == 1, f"expected 1 occurrence, found {vcontent.count(old_ver_line)}"
vcontent = vcontent.replace(old_ver_line, new_ver_line)

with open(vpath, "w") as f:
    f.write(vcontent)

print(f"Bumped version.h: {old_ver_line} -> {new_ver_line}")
EOF
