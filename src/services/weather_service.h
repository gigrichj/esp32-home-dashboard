#pragma once
#include <Arduino.h>

struct WeatherData {
  float tempF = 0;
  float feelsLikeF = 0;
  String condition = "--";
  float windMph = 0;
  int humidity = 0;
  bool valid = false;
  // hourly[] / daily[] arrays would extend this struct once the
  // basic "current conditions" call is working end-to-end.
};

extern WeatherData g_weather;

// Fetches current conditions from OpenWeatherMap (or swap for NWS api.weather.gov).
void weather_service_update();
