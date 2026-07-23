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
};

static const uint32_t TREND_SAMPLE_INTERVAL_MS = 5UL * 60UL * 1000UL; // 5 min
static const int TREND_MAX_SAMPLES = 288; // 24h at 5-min spacing

extern TrendSample g_trendSamples[TREND_MAX_SAMPLES];
extern int g_trendSampleCount;   // how many valid samples exist (caps at TREND_MAX_SAMPLES)
extern int g_trendNextWriteIdx;  // ring buffer write position

// Call regularly (e.g. every networkTask iteration) -- internally gates
// itself to TREND_SAMPLE_INTERVAL_MS, so it's a cheap no-op most calls.
void trend_history_update();
