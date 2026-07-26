#pragma once
#include <Arduino.h>

// Rolling ~24h history of a few headline metrics, sampled periodically so
// the Trends page can draw sparkline-style history instead of only ever
// showing "right now". Deliberately lightweight (plain floats/ints, no
// Strings) since this buffer stays resident in RAM for the device's
// entire uptime.
struct TrendSample {
  uint32_t unixTime = 0;
  float tempF = 0;
  int aqi = 0;            // 0 = no data yet at sample time
  int aircraftCount = 0;
  float astroBadness = -1; // 0..1, lower is better; -1 = no astro data yet
  float uvIndex = -1;      // -1 = no data yet at sample time (0 is a legitimate reading, e.g. at night)

  // Fetch health -- minutes since each fetch type last succeeded, sampled
  // at the same 5-min cadence as everything else above. -1 = that fetch
  // has never succeeded since boot. Normally sits near 0 (succeeding every
  // cycle); climbs during a real outage (WiFi drop, API down) and drops
  // back to 0 once it recovers -- a more informative signal than a flat
  // success/fail line, which would mostly just read "1" the whole time.
  float weatherStaleMin = -1;
  float airQualityStaleMin = -1;
  float astroStaleMin = -1;
  float precipStaleMin = -1;
  float aviationStaleMin = -1;
  float issStaleMin = -1;
  float spacexStaleMin = -1;
  float spacexDetailStaleMin = -1;
};

static const uint32_t TREND_SAMPLE_INTERVAL_MS = 5UL * 60UL * 1000UL; // 5 min
static const int TREND_MAX_SAMPLES = 288; // 24h at 5-min spacing

extern TrendSample g_trendSamples[TREND_MAX_SAMPLES];
extern int g_trendSampleCount;   // how many valid samples exist (caps at TREND_MAX_SAMPLES)
extern int g_trendNextWriteIdx;  // ring buffer write position

// Last-success millis() timestamp for each of the 8 tracked fetch types,
// set by main.cpp's networkTask right alongside each fetch's existing
// success check. 0 = never succeeded since boot. trend_history_update()
// reads these to compute the staleness fields above.
extern uint32_t g_lastWeatherSuccessMs;
extern uint32_t g_lastAirQualitySuccessMs;
extern uint32_t g_lastAstroSuccessMs;
extern uint32_t g_lastPrecipSuccessMs;
extern uint32_t g_lastAviationSuccessMs;
extern uint32_t g_lastIssSuccessMs;
extern uint32_t g_lastSpacexSuccessMs;
extern uint32_t g_lastSpacexDetailSuccessMs;

// Call regularly (e.g. every networkTask iteration) -- internally gates
// itself to TREND_SAMPLE_INTERVAL_MS, so it's a cheap no-op most calls.
void trend_history_update();
