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
  int humidity = 0;
  int weatherId = 0;         // OWM condition code, drives icon selection
  uint32_t sunriseUnix = 0;
  uint32_t sunsetUnix = 0;
  bool valid = false;
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
