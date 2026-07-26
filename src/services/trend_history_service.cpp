#include "trend_history_service.h"
#include "weather_service.h"
#include "air_quality_service.h"
#include "aviation_service.h"
#include "astro_seeing_service.h"
#include <time.h>
#include "../state_mutex.h"

TrendSample g_trendSamples[TREND_MAX_SAMPLES];
int g_trendSampleCount = 0;
int g_trendNextWriteIdx = 0;

uint32_t g_lastWeatherSuccessMs = 0;
uint32_t g_lastAirQualitySuccessMs = 0;
uint32_t g_lastAstroSuccessMs = 0;
uint32_t g_lastPrecipSuccessMs = 0;
uint32_t g_lastAviationSuccessMs = 0;
uint32_t g_lastIssSuccessMs = 0;
uint32_t g_lastSpacexSuccessMs = 0;
uint32_t g_lastSpacexDetailSuccessMs = 0;

// 0 (never succeeded) -> -1 sentinel (no data yet), same convention as
// astroBadness above. Otherwise minutes since that fetch last succeeded.
static float staleMinutesSince(uint32_t lastSuccessMs, uint32_t nowMs) {
  if (lastSuccessMs == 0) return -1.0f;
  return (float)(nowMs - lastSuccessMs) / 60000.0f;
}

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

  s.weatherStaleMin = staleMinutesSince(g_lastWeatherSuccessMs, nowMs);
  s.airQualityStaleMin = staleMinutesSince(g_lastAirQualitySuccessMs, nowMs);
  s.astroStaleMin = staleMinutesSince(g_lastAstroSuccessMs, nowMs);
  s.precipStaleMin = staleMinutesSince(g_lastPrecipSuccessMs, nowMs);
  s.aviationStaleMin = staleMinutesSince(g_lastAviationSuccessMs, nowMs);
  s.issStaleMin = staleMinutesSince(g_lastIssSuccessMs, nowMs);
  s.spacexStaleMin = staleMinutesSince(g_lastSpacexSuccessMs, nowMs);
  s.spacexDetailStaleMin = staleMinutesSince(g_lastSpacexDetailSuccessMs, nowMs);

  // Locked as one block -- g_trendSampleCount/g_trendNextWriteIdx and
  // g_trendSamples[] must never be visible to a reader (the Trends page
  // draw code) mid-update.
  state_lock();
  g_trendSamples[g_trendNextWriteIdx] = s;
  g_trendNextWriteIdx = (g_trendNextWriteIdx + 1) % TREND_MAX_SAMPLES;
  if (g_trendSampleCount < TREND_MAX_SAMPLES) {
    g_trendSampleCount++;
  }
  state_unlock();
}
