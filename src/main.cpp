#include <Arduino.h>
#include "secrets.h"
#include "panel_display.h"
#include "services/wifi_manager.h"
#include "services/mqtt_service.h"
#include "services/weather_service.h"
#include "services/aviation_service.h"
#include "services/iss_service.h"
#include "services/smarthome_service.h"
#include "screens/screen_manager.h"

using namespace PanelDisplay;

// Poll intervals (ms) — stagger these so the ESP32 isn't hammering
// multiple HTTPS endpoints in the same loop iteration.
static const uint32_t WEATHER_POLL_MS    = 10UL * 60UL * 1000UL; // 10 min
static const uint32_t AVIATION_POLL_MS   = 15UL * 1000UL;        // 15 sec
static const uint32_t ISS_POLL_MS        = 60UL * 1000UL;        // 1 min
static const uint32_t SMARTHOME_POLL_MS  = 5UL * 1000UL;         // 5 sec
static const uint32_t DRAW_INTERVAL_MS   = 200UL;                // ~5 fps

uint32_t lastWeather = 0, lastAviation = 0, lastIss = 0, lastSmartHome = 0, lastDraw = 0;

void setup() {
  Serial.begin(115200);
  uint32_t serialStart = millis();
  while (!Serial && millis() - serialStart < 3000) {
    delay(20);
  }

  Serial.println("[boot] display begin");
  if (!screen.begin()) {
    Serial.println("[boot] display FAILED — halting");
    while (true) delay(1000);
  }
  Serial.println("[boot] display ready");

  screen_manager_init();

  wifi_manager_begin(WIFI_SSID, WIFI_PASSWORD);
  mqtt_service_begin();

  // Initial data fetch so screens aren't empty on boot
  weather_service_update();
  aviation_service_update();
  iss_service_update();
  smarthome_service_update();

  screen_manager_draw();
  screen.present();
}

void loop() {
  wifi_manager_loop();
  mqtt_service_loop();

  uint16_t touchX = 0, touchY = 0;
  bool touched = screen.readTouch(&touchX, &touchY);
  screen_manager_handle_touch(touched, touchX, touchY);

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

  if (now - lastDraw > DRAW_INTERVAL_MS) {
    lastDraw = now;
    screen_manager_draw();
    if (!screen.present()) {
      Serial.println("[loop] present failed; restarting");
      Serial.flush();
      delay(100);
      ESP.restart();
    }
  }

  delay(1);
}
