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

static const uint32_t WEATHER_POLL_MS    = 10UL * 60UL * 1000UL; // 10 min
static const uint32_t AVIATION_POLL_MS   = 15UL * 1000UL;        // 15 sec
static const uint32_t ISS_POLL_MS        = 60UL * 1000UL;        // 1 min
static const uint32_t SMARTHOME_POLL_MS  = 5UL * 1000UL;         // 5 sec
static const uint32_t DRAW_INTERVAL_MS   = 200UL;                // ~5 fps

uint32_t lastWeather = 0, lastAviation = 0, lastIss = 0, lastSmartHome = 0, lastDraw = 0;
bool wasInSetupMode = false;

static void draw_setup_screen() {
  uint16_t bg = screen.color565(10, 12, 16);
  uint16_t text = screen.color565(235, 240, 245);
  uint16_t accent = screen.color565(70, 130, 220);

  screen.fillScreen(bg);
  screen.setTextSize(3);
  screen.setTextColor(accent, bg);
  screen.setTextDatum(textdatum_t::top_left);
  screen.drawString("WIFI SETUP NEEDED", 30, 60);

  screen.setTextSize(2);
  screen.setTextColor(text, bg);
  screen.drawString("1. On your phone, connect to WiFi network:", 30, 140);
  screen.setTextColor(accent, bg);
  screen.drawString("   ESP32-Dashboard-Setup", 30, 175);

  screen.setTextColor(text, bg);
  screen.drawString("2. Open a browser and go to:", 30, 230);
  screen.setTextColor(accent, bg);
  screen.drawString("   http://192.168.4.1", 30, 265);

  screen.setTextColor(text, bg);
  screen.drawString("3. Enter your home WiFi name and password.", 30, 320);
  screen.drawString("   The device will restart and connect.", 30, 350);
}

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

  wifi_manager_begin();

  if (wifi_manager_in_setup_mode()) {
    draw_setup_screen();
    screen.present();
    wasInSetupMode = true;
    return; // don't start services/data fetching until WiFi is configured
  }

  mqtt_service_begin();

  weather_service_update();
  aviation_service_update();
  iss_service_update();
  smarthome_service_update();

  screen_manager_draw();
  screen.present();
}

void loop() {
  wifi_manager_loop();

  if (wifi_manager_in_setup_mode()) {
    // Still waiting for the person to submit WiFi credentials through
    // the setup portal — keep serving that instead of the dashboard.
    delay(10);
    return;
  }

  if (wasInSetupMode) {
    // Just came out of setup mode this loop (shouldn't normally happen
    // without a restart, but handle it gracefully just in case).
    wasInSetupMode = false;
    mqtt_service_begin();
  }

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
