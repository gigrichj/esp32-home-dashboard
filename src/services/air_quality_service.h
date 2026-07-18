#pragma once
#include <Arduino.h>

struct AirQualityData {
  int aqi = 0;           // OpenWeatherMap AQI index: 1=Good .. 5=Very Poor
  float pm2_5 = 0;
  float pm10 = 0;
  bool valid = false;
};

extern AirQualityData g_airQuality;

// Fetches current air quality index from OpenWeatherMap's Air Pollution API,
// using the same lat/lon/API key as weather_service.
void air_quality_service_update();

// Short label for an OWM AQI index ("Good", "Fair", "Moderate", "Poor", "Very Poor").
const char* air_quality_label(int aqi);
