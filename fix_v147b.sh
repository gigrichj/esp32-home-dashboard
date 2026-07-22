#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/screens/screen_manager.cpp"
with open(path) as f:
    content = f.read()

old = "static void drawCloudIcon(int cx, int cy, int r, uint16_t color); // defined further down"
assert content.count(old) == 1, f"expected 1 occurrence, found {content.count(old)}"
new = old + "\nstatic void drawWeatherBackground(int weatherId, bool isNight); // defined further down"
content = content.replace(old, new)

with open(path, "w") as f:
    f.write(content)
print("Added forward declaration for drawWeatherBackground()")
EOF
