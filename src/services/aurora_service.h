#pragma once
#include <Arduino.h>

// Planetary Kp index is a single global value -- it doesn't vary by
// observer location the way weather does. Lorton, VA and Spruce Knob, WV
// are close enough in geomagnetic latitude (~230mi apart) that the same
// Kp value and the same rough visibility threshold apply to both, so one
// fetch covers both locations instead of needing a per-site request.

// Most recent observed Kp reading (3-hour period), from NOAA SWPC.
extern float g_currentKp;
extern uint32_t g_currentKpPeriodEndUnix; // end of the 3-hour period this Kp covers
extern bool g_kpObservedValid;

// 3-day Kp forecast, one value per 3-hour period (NOAA only forecasts out
// 3 days for Kp -- there is no 5-day Kp forecast product).
static const int KP_FORECAST_MAX_POINTS = 24; // 3 days * 8 periods/day
struct KpForecastPoint {
  uint32_t periodStartUnix = 0;
  float kp = 0;
};
extern KpForecastPoint g_kpForecast[KP_FORECAST_MAX_POINTS];
extern int g_kpForecastCount;
extern bool g_kpForecastValid;

extern int g_auroraLastHttpCode;
extern String g_auroraLastFailureReason;

// Fetches current + forecast planetary Kp index from NOAA SWPC (free, no
// API key). Two endpoints: noaa-planetary-k-index.json (observed) and
// noaa-planetary-k-index-forecast.json (3-day forecast).
void aurora_service_update();

// Rough naked-eye aurora visibility threshold for mid-Atlantic latitudes
// (~38-39N geomagnetic) -- this is NOT a precise physical model, just a
// commonly-cited rule of thumb: Kp 7+ is the usual threshold for a
// _chance_ of visible aurora this far south, Kp 8-9 for a good chance.
// Returns a short label ("Unlikely", "Slight Chance", "Possible", "Likely").
const char* aurora_visibility_label(float kp);
