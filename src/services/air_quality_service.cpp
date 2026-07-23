#include "air_quality_service.h"
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <ArduinoJson.h>

AirQualityData g_airQuality;

const char* air_quality_label(int aqi) {
  switch (aqi) {
    case 1: return "Good";
    case 2: return "Fair";
    case 3: return "Moderate";
    case 4: return "Poor";
    case 5: return "Very Poor";
    default: return "--";
  }
}

// Same yield-safe manual read loop established in aviation_service.cpp's
// fetchAircraftFromUrl() -- this function previously used plain
// http.getString(), which blocks with no yields and was the root cause
// of the original FreeRTOS task watchdog crash (fixed everywhere else in
// v128, but this fetch was apparently never converted). A stalled or
// reset connection during getString() can block long enough to trip the
// watchdog and reboot the device -- consistent with air quality data
// being missing right before a crash.
void air_quality_service_update() {
  if (!wifi_manager_is_connected()) return;

  HTTPClient http;
  char url[256];
  snprintf(url, sizeof(url),
    "https://api.openweathermap.org/data/2.5/air_pollution?lat=%f&lon=%f&appid=%s",
    (double)HOME_LAT, (double)HOME_LON, OWM_API_KEY);

  http.begin(url);
  int code = http.GET();
  g_airQuality.lastHttpCode = code;

  if (code != 200) {
    Serial.printf("[AirQuality] HTTP %d\n", code);
    http.end();
    return;
  }

  int payloadLen = http.getSize();
  if (payloadLen > 20000) {
    Serial.printf("[AirQuality] payload implausibly large (%d bytes), skipping\n", payloadLen);
    http.end();
    return;
  }

  int bufSize = (payloadLen > 0) ? payloadLen + 1 : 8192;
  char *rawBuf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
  if (rawBuf == nullptr) {
    Serial.println("[AirQuality] payload buffer alloc failed");
    http.end();
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t readTotal = 0;
  uint32_t startMs = millis();
  bool readError = false;
  while (readTotal < (size_t)(bufSize - 1) && millis() - startMs < 8000) {
    if (!http.connected() && stream->available() == 0) break;
    size_t avail = stream->available();
    if (avail > 0) {
      int toRead = (int)min(avail, (size_t)(bufSize - 1 - readTotal));
      int r = stream->readBytes(rawBuf + readTotal, toRead);
      if (r <= 0) { readError = true; break; }
      readTotal += r;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5)); // yield -- same critical fix as aviation_service.cpp's v128
    }
  }
  rawBuf[readTotal] = '\0';
  http.end();

  if (readError) {
    Serial.println("[AirQuality] payload read error");
    free(rawBuf);
    return;
  }

  String payload(rawBuf);
  free(rawBuf);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (!err) {
    JsonObject item = doc["list"][0];
    g_airQuality.aqi   = item["main"]["aqi"] | 0;
    g_airQuality.pm2_5 = item["components"]["pm2_5"] | 0.0f;
    g_airQuality.pm10  = item["components"]["pm10"]  | 0.0f;
    g_airQuality.valid = true;
  } else {
    Serial.printf("[AirQuality] JSON parse error: %s\n", err.c_str());
  }
}
