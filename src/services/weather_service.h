#pragma once
#include <Arduino.h>

struct WeatherData {
  float tempF = 0;
  float feelsLikeF = 0;
  String condition = "--";
  float windMph = 0;
  float windGustMph = 0;
  float windDeg = 0;         // meteorological direction, 0=N, clockwise
  int precipChance = 0;      // 0-100, from the nearest forecast entry's "pop"
  float dewPointF = 0;       // calculated locally (Magnus formula) -- OWM's
                              // free current-conditions endpoint doesn't
                              // include this, only the paid One Call API does
  int humidity = 0;
  int weatherId = 0;         // OWM condition code, drives icon selection
  uint32_t sunriseUnix = 0;
  uint32_t sunsetUnix = 0;
  bool valid = false;
  int lastHttpCode = -999; // shown on-screen if fetch never succeeds
  String debugSunLine = ""; // TEMP DEBUG (v113): font-safe sunrise/sunset trace

  // UV index -- not available on OpenWeatherMap's free tier (paid One Call
  // API only), so fetched separately from Open-Meteo (same source already
  // used as the Astro page's 7Timer fallback).
  float uvIndex = 0;
  bool uvValid = false;
  int uvLastHttpCode = -999;
};

// 24-hour rolling precipitation-probability forecast, from Open-Meteo
// (same source already used for the Astro page's 7Timer fallback and
// the UV index above) -- OpenWeatherMap's forecast used for g_weather
// is only 3-hour resolution and only surfaces a single "pop" per call,
// not a full next-24-hours view.
struct HourlyPrecipPoint {
  uint32_t unixTime = 0;
  int precipProb = 0; // 0-100
};

static const int PRECIP_HOURLY_POINTS = 24;
extern HourlyPrecipPoint g_precipHourly[PRECIP_HOURLY_POINTS];
extern int g_precipHourlyCount;
extern bool g_precipHourlyValid;
extern int g_precipHourlyLastHttpCode;

struct ForecastDay {
  String dayLabel = "";      // "Mon", "Tue", etc.
  float highF = -999.0f;
  float lowF = 999.0f;
  int weatherId = 0;
};

static const int FORECAST_DAYS = 5;
extern ForecastDay g_forecast[FORECAST_DAYS];
extern int g_forecastCount;

extern WeatherData g_weather;

// Fetches current conditions + a 5-day forecast from OpenWeatherMap.
void weather_service_update();

// Fetches just the 24-hour precip forecast on its own, independent of
// the rest of the bundle above -- lets main.cpp retry it on a faster
// schedule if it fails without re-triggering the heavier current-
// conditions/forecast/UV fetches too.
void weather_service_update_precip_only();
