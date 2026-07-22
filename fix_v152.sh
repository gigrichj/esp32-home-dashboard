#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/screens/screen_manager.cpp"
with open(path) as f:
    content = f.read()

old = '''  int cx = WIDTH / 2;
  int shieldTopCy = 210;
  int shieldR = 95;
  int apexY = 400;'''
assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"
new = '''  int cx = WIDTH / 2;
  int shieldR = 140;      // scaled up (was 95) so the badge fills the screen top-to-bottom
  int shieldTopCy = 20 + shieldR;  // top of the rounded arc sits ~20px from the screen's top edge
  int apexY = HEIGHT - 20;         // point of the shield sits ~20px from the screen's bottom edge'''
content = content.replace(old, new)

old2 = '''  int compassCy = shieldTopCy + 40;
  int compassR = 70;'''
assert content.count(old2) == 1, f"expected 1 occurrence, found {content.count(old2)}"
new2 = '''  int compassCy = shieldTopCy + 59;  // same proportion as before, scaled to the larger shield
  int compassR = 103;'''
content = content.replace(old2, new2)

old3 = '''  int orbitR = 42;'''
assert content.count(old3) == 1, f"expected 1 occurrence, found {content.count(old3)}"
new3 = '''  int orbitR = 62;'''
content = content.replace(old3, new3)

old4 = '''  int starOuterR = 20, starInnerR = 9;'''
assert content.count(old4) == 1, f"expected 1 occurrence, found {content.count(old4)}"
new4 = '''  int starOuterR = 29, starInnerR = 13;'''
content = content.replace(old4, new4)

with open(path, "w") as f:
    f.write(content)
print("Scaled the debug badge up to fill the screen top-to-bottom")

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
