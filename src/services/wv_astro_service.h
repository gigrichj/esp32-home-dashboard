#pragma once
#include <Arduino.h>

// Astro forecast for Spruce Knob, WV (Bortle 2 dark-sky site, see
// WV_BORTLE_CLASS in secrets.h) -- same data shape as
// astro_seeing_service.h's AstroForecastPoint, but a separate fetch/array
// since this queries a different lat/lon (WV_LAT/WV_LON) than the home
// astro page. Moon phase is NOT recalculated here -- the moon is the same
// everywhere on Earth, so the WV page reuses astro_seeing_service.h's
// g_moonPhaseFraction/g_moonIllumPercent/etc. directly.
struct WvAstroForecastPoint {
  uint32_t unixTime = 0;
  int cloudcover = 0;      // 1-9 index, 1=clearest, 9=overcast
  int seeing = 0;          // 1-8 index, 1=best (<0.5"), 8=worst (>2.5")
  int transparency = 0;    // 1-8 index, 1=best, 8=worst
  int liftedindex = 0;     // instability; more negative = higher storm risk
  String prectype = "";    // "none", "rain", "snow", etc.
};

// 7Timer's ASTRO product forecasts up to 3 real days at 3-hour spacing
// (24 points). Unlike the home Astro page (which only keeps 16 points,
// ~48h), this keeps the full 3 days -- the whole point of this page is
// multi-day trip planning.
static const int WV_ASTRO_MAX_POINTS = 24;
extern WvAstroForecastPoint g_wvAstroForecast[WV_ASTRO_MAX_POINTS];
extern int g_wvAstroForecastCount;

// Days 4-5, beyond 7Timer's 3-day window: Open-Meteo cloud-cover-only,
// since no free source provides real seeing/transparency data this far
// out. One entry per night, averaged over an approximate nighttime
// window (02:00-08:00 UTC, roughly 9pm-3am Eastern -- doesn't shift with
// DST, treat as approximate). Deliberately lower-confidence than the
// 7Timer days and shown differently on the WV page.
struct WvCloudOnlyNight {
  uint32_t nightDateUnix = 0;   // UTC midnight of the night's start date, for labeling
  float avgCloudcoverPct = -1;  // -1 = no data for this night
};
static const int WV_CLOUD_ONLY_NIGHTS = 2;
extern WvCloudOnlyNight g_wvCloudOnlyNights[WV_CLOUD_ONLY_NIGHTS];
extern bool g_wvCloudOnlyValid;

extern int g_wvAstroLastHttpCode;
extern String g_wvAstroLastFailureReason;

// Fetches BOTH sources every time (unlike the home Astro page, which
// treats Open-Meteo as an all-or-nothing fallback only used when 7Timer
// fails entirely). 7Timer covers days 1-3 with real seeing/transparency;
// Open-Meteo extends days 4-5 with cloud-cover-only data. If 7Timer
// fails, days 1-3 show "no data" rather than silently falling back to
// estimated seeing/transparency -- a wasted trip to Spruce Knob is a
// bigger cost than a page that says "check back later."
void wv_astro_service_update();
