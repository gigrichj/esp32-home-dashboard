#!/usr/bin/env bash
set -e

python3 << 'EOF'
path = "src/services/iss_service.cpp"
with open(path) as f:
    content = f.read()

# --- Add new static tracking vars next to the TLE ones ---
old_vars = '''static Sgp4 sat;
static bool tleLoaded = false;
static uint32_t lastTleFetchMs = 0;
static const uint32_t TLE_REFRESH_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL; // 6 hours'''
assert content.count(old_vars) == 1, f"vars block: expected 1, found {content.count(old_vars)}"
new_vars = '''static Sgp4 sat;
static bool tleLoaded = false;
static uint32_t lastTleFetchMs = 0;
static const uint32_t TLE_REFRESH_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL; // 6 hours

// Crew count is decoupled from the TLE refresh -- poll every 60s until we
// have a first successful value, then settle into the same 6-hour cadence
// as the TLE (crew rotations are infrequent, no need to check more often).
static bool crewCountLoaded = false;
static uint32_t lastCrewFetchMs = 0;
static const uint32_t CREW_REFRESH_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL; // 6 hours
static const uint32_t CREW_REFRESH_RETRY_MS = 60UL * 1000UL; // 60 seconds, until first success'''
content = content.replace(old_vars, new_vars)

# --- Mark crewCountLoaded = true on a successful fetch ---
old_fetch = '''      g_issCrewCount = count;
    }
  } else {
    Serial.printf("[ISS] astros.json HTTP %d\\n", code);
  }
  http.end();
}'''
assert content.count(old_fetch) == 1, f"fetch success block: expected 1, found {content.count(old_fetch)}"
new_fetch = '''      g_issCrewCount = count;
      crewCountLoaded = true;
    }
  } else {
    Serial.printf("[ISS] astros.json HTTP %d\\n", code);
  }
  http.end();
}'''
content = content.replace(old_fetch, new_fetch)

# --- Decouple the call site from the TLE refresh block ---
old_call = '''  if (!tleLoaded || millis() - lastTleFetchMs > TLE_REFRESH_INTERVAL_MS) {
    if (fetchAndInitTLE()) {
      tleLoaded = true;
      lastTleFetchMs = millis();
    }
    fetchCrewCount();
  }
  computeGroundTrack();'''
assert content.count(old_call) == 1, f"call site: expected 1, found {content.count(old_call)}"
new_call = '''  if (!tleLoaded || millis() - lastTleFetchMs > TLE_REFRESH_INTERVAL_MS) {
    if (fetchAndInitTLE()) {
      tleLoaded = true;
      lastTleFetchMs = millis();
    }
  }

  uint32_t crewRefreshInterval = crewCountLoaded ? CREW_REFRESH_INTERVAL_MS : CREW_REFRESH_RETRY_MS;
  if (!crewCountLoaded || millis() - lastCrewFetchMs > crewRefreshInterval) {
    lastCrewFetchMs = millis();
    fetchCrewCount();
  }

  computeGroundTrack();'''
content = content.replace(old_call, new_call)

with open(path, "w") as f:
    f.write(content)
print("Decoupled crew count refresh from TLE refresh: 60s retry until loaded, then 6h cadence")

# --- bump version ---
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
