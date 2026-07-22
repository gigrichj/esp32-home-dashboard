#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/screens/screen_manager.cpp"
with open(path) as f:
    content = f.read()

old = '''      const char* labels[4] = {"Stable", "Slight Risk", "Moderate Risk", "High Risk"};'''
assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"
new = '''      const char* labels[4] = {"Stable", "Slight", "Moderate", "or High Risk"};'''
content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Updated Storm Risk legend wording to (Stable, Slight, Moderate, or High Risk)")

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
