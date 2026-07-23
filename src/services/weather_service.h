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
