#include "trend_history_service.h"
#include "weather_service.h"
#include "air_quality_service.h"
#include "aviation_service.h"
#include "astro_seeing_service.h"
#include <time.h>

TrendSample g_trendSamples[TREND_MAX_SAMPLES];
int g_trendSampleCount = 0;
int g_trendNextWriteIdx = 0;

void trend_history_update() {
  static uint32_t lastSampleMs = 0;
  uint32_t nowMs = millis();

  // First call after boot: sample immediately rather than waiting a full
  // interval, so the Trends page has at least one point right away.
  if (lastSampleMs != 0 && nowMs - lastSampleMs < TREND_SAMPLE_INTERVAL_MS) {
    return;
  }
  lastSampleMs = nowMs;

  TrendSample s;
  s.unixTime = (uint32_t)time(nullptr);
  s.tempF = g_weather.valid ? g_weather.tempF : 0.0f;
  s.aqi = g_airQuality.valid ? g_airQuality.aqi : 0;
  s.aircraftCount = g_aircraftCount;

  // Astro badness: reuse the same composite score the Astro/Dashboard
  // pages already compute, based on whichever forecast point is nearest
  // to right now (index 0 -- Open-Meteo/7Timer forecasts start from the
  // current hour going forward), rather than duplicating that logic.
  if (g_astroForecastCount > 0) {
    float badness = 0;
    astro_tonight_verdict(g_astroForecast[0].cloudcover, g_astroForecast[0].seeing,
                           g_astroForecast[0].transparency, g_moonIllumPercent, &badness);
    s.astroBadness = badness;
  } else {
    s.astroBadness = -1;
  }

  g_trendSamples[g_trendNextWriteIdx] = s;
  g_trendNextWriteIdx = (g_trendNextWriteIdx + 1) % TREND_MAX_SAMPLES;
  if (g_trendSampleCount < TREND_MAX_SAMPLES) {
    g_trendSampleCount++;
  }
}
