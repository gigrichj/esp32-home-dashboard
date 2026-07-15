#include <Arduino.h>
#include <lvgl.h>
#include "secrets.h"
#include "config/display_config.h"
#include "services/wifi_manager.h"
#include "services/mqtt_service.h"
#include "services/weather_service.h"
#include "services/aviation_service.h"
#include "services/iss_service.h"
#include "services/smarthome_service.h"
#include "screens/screen_manager.h"

// Poll intervals (ms) — stagger these so the ESP32 isn't hammering
// multiple HTTPS endpoints in the same loop iteration.
static const uint32_t WEATHER_POLL_MS    = 10UL * 60UL * 1000UL; // 10 min
static const uint32_t AVIATION_POLL_MS   = 15UL * 1000UL;        // 15 sec
static const uint32_t ISS_POLL_MS        = 60UL * 1000UL;        // 1 min
static const uint32_t SMARTHOME_POLL_MS  = 5UL * 1000UL;         // 5 sec

uint32_t lastWeather = 0, lastAviation = 0, lastIss = 0, lastSmartHome = 0;

void setup() {
  Serial.begin(115200);

  display_init();        // LVGL + panel + touch init (config/display_config.*)
  screen_manager_init(); // build tab views: dashboard, aviation, porsche, smarthome, iss, weather

  wifi_manager_begin(WIFI_SSID, WIFI_PASSWORD);
  mqtt_service_begin();  // subscribes to HA/Hubitat notification topics

  // Initial data fetch so screens aren't empty on boot
  weather_service_update();
  aviation_service_update();
  iss_service_update();
  smarthome_service_update();
}

void loop() {
  lv_timer_handler();
  wifi_manager_loop();
  mqtt_service_loop();

  uint32_t now = millis();

  if (now - lastWeather > WEATHER_POLL_MS) {
    lastWeather = now;
    weather_service_update();
  }
  if (now - lastAviation > AVIATION_POLL_MS) {
    lastAviation = now;
    aviation_service_update();
  }
  if (now - lastIss > ISS_POLL_MS) {
    lastIss = now;
    iss_service_update();
  }
  if (now - lastSmartHome > SMARTHOME_POLL_MS) {
    lastSmartHome = now;
    smarthome_service_update();
  }

  screen_manager_refresh();
  delay(5);
}
