#pragma once
#include <Arduino.h>

struct AstroForecastPoint {
  uint32_t unixTime = 0;
  int cloudcover = 0;      // 1-9 index, 1=clearest, 9=overcast
  int seeing = 0;          // 1-8 index, 1=best (<0.5"), 8=worst (>2.5")
  int transparency = 0;    // 1-8 index, 1=best, 8=worst
  int liftedindex = 0;     // instability; more negative = higher storm risk
  String prectype = "";    // "none", "rain", "snow", etc.
};

static const int ASTRO_MAX_POINTS = 16; // ~48 hours at 3-hour spacing

extern AstroForecastPoint g_astroForecast[ASTRO_MAX_POINTS];
extern int g_astroForecastCount;

// Last HTTP result from the 7Timer fetch (-999 = never attempted), shown
// on-screen when there's no forecast data so troubleshooting doesn't
// require a serial monitor or waiting on the debug log.
extern int g_astroLastHttpCode;

// Moon phase/illumination, calculated locally from the date -- no network
// call needed, accurate to well within a day.
extern float g_moonPhaseFraction;   // 0=new, 0.5=full, 1=new again
extern float g_moonIllumPercent;    // 0-100
extern String g_moonPhaseLabel;

// Fetches the 7Timer! ASTRO forecast (free, no API key) and recalculates
// the current moon phase.
void astro_seeing_service_update();

// Recalculates just the moon phase from the current time. Cheap (pure
// math, no network), so it's safe to call every time the astro page is
// drawn -- this decouples the moon phase from the network fetch timing,
// since the very first fetch attempt at boot can happen before the clock
// has finished syncing, silently skipping the calculation until the next
// scheduled poll (up to ASTRO_POLL_MS later).
void astro_recompute_moon_phase();

// Index -> human-readable label helpers (kept out of the display code so
// the mapping tables live in one place).
const char* astro_seeing_label(int idx);
const char* astro_transparency_label(int idx);
const char* astro_cloudcover_label(int idx);
const char* astro_instability_label(int liftedIndex);

// Composite go/no-go verdict combining cloud cover, seeing, transparency,
// and moon brightness -- cloud weighted heaviest, then seeing, then
// transparency and moon equally, matching how amateur-astronomy tools
// like AstroWeather typically combine these factors. Writes the
// aggregate badness (0=best, 1=worst) to outBadness if non-null, for
// color-coding the verdict on-screen.
const char* astro_tonight_verdict(int cloudcover, int seeing, int transparency,
                                   float moonIllumPercent, float* outBadness);
