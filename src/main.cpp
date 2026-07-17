#include <Arduino.h>
#include "secrets.h"
#include "panel_display.h"
#include "version.h"
#include "services/wifi_manager.h"
#include "services/mqtt_service.h"
#include "services/weather_service.h"
#include "services/aviation_service.h"
#include "services/iss_service.h"
#include "services/smarthome_service.h"
#include "screens/screen_manager.h"
#include "debug_log.h"

using namespace PanelDisplay;

static const uint32_t WEATHER_POLL_MS    = 10UL * 60UL * 1000UL;
static const uint32_t AVIATION_POLL_MS   = 60UL * 1000UL;
static const uint32_t ISS_POLL_MS        = 60UL * 1000UL;
static const uint32_t SMARTHOME_POLL_MS  = 5UL * 1000UL;
static const uint32_t DRAW_INTERVAL_MS   = 200UL;

bool wasInSetupMode = false;
bool setupModeActive = false;

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

  String lastSsid = wifi_manager_last_attempted_ssid();
  if (lastSsid.length() > 0) {
    screen.setTextSize(1);
    screen.setTextColor(screen.color565(200, 90, 90), bg);
    char line[96];
    snprintf(line, sizeof(line), "Last attempt: '%s' -> status code %d",
             lastSsid.c_str(), wifi_manager_last_status_code());
    screen.drawString(line, 30, 420);
    screen.drawString("(3=connected, 1=no network found, 4=connect failed/bad password, 6=disconnected)", 30, 440);
  }

  screen.setTextSize(1);
  screen.setTextColor(screen.color565(90, 100, 110), bg);
  screen.setTextDatum(textdatum_t::top_right);
  screen.drawString(FIRMWARE_VERSION, WIDTH - 6, HEIGHT - 14);
}

void uiTask(void* param) {
  for (;;) {
    if (setupModeActive) {
      draw_setup_screen();
      screen.present();
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    uint16_t touchX = 0, touchY = 0;
    bool touched = screen.readTouch(&touchX, &touchY);
    screen_manager_handle_touch(touched, touchX, touchY);

    screen_manager_draw();

    uint16_t dbgBg = screen.color565(0, 0, 0);
    uint16_t dbgText = touched ? screen.color565(80, 220, 100) : screen.color565(120, 120, 120);
    screen.fillRect(0, HEIGHT - 24, 240, 24, dbgBg);
    screen.setTextSize(1);
    screen.setTextColor(dbgText, dbgBg);
    screen.setTextDatum(textdatum_t::top_left);
    char touchDbg[64];
    if (!screen.touchAvailable()) {
      snprintf(touchDbg, sizeof(touchDbg), "TOUCH: controller NOT initialized");
    } else {
      snprintf(touchDbg, sizeof(touchDbg), "TOUCH: x=%d y=%d count=%d",
               touchX, touchY, screen.lastTouchReadCount());
    }
    screen.drawString(touchDbg, 6, HEIGHT - 18);

    if (!screen.present()) {
      Serial.println("[uiTask] present failed; restarting");
      Serial.flush();
      delay(100);
      ESP.restart();
    }

    vTaskDelay(pdMS_TO_TICKS(DRAW_INTERVAL_MS));
  }
}

void networkTask(void* param) {
  uint32_t lastWeather = 0, lastAviation = 0, lastIss = 0, lastSmartHome = 0;

  for (;;) {
    wifi_manager_loop();
    setupModeActive = wifi_manager_in_setup_mode();

    if (setupModeActive) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (wasInSetupMode) {
      wasInSetupMode = false;
      mqtt_service_begin();
    }

    mqtt_service_loop();

    uint32_t now = millis();

    if (now - lastWeather > WEATHER_POLL_MS) {
      lastWeather = now;
      debug_log("weather fetch start");
      weather_service_update();
      debug_log("weather fetch done");
    }
    if (now - lastAviation > AVIATION_POLL_MS) {
      lastAviation = now;
      debug_log("aviation fetch start");
      aviation_service_update();
      debug_log("aviation fetch done");
    }
    if (now - lastIss > ISS_POLL_MS) {
      lastIss = now;
      debug_log("iss fetch start");
      iss_service_update();
      debug_log("iss fetch done");
    }


    vTaskDelay(pdMS_TO_TICKS(10));
  }
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
  setupModeActive = wifi_manager_in_setup_mode();

  xTaskCreatePinnedToCore(uiTask, "uiTask", 8192, nullptr, 1, nullptr, 1);

  if (!setupModeActive) {
    mqtt_service_begin();
    configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
    weather_service_update();
    aviation_service_update();
    iss_service_update();
  } else {
    wasInSetupMode = true;
  }

  xTaskCreatePinnedToCore(networkTask, "networkTask", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
