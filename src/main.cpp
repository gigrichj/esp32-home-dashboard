#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include "secrets.h"
#include "panel_display.h"
#include "version.h"
#include "services/weather_service.h"
#include "services/aviation_service.h"
#include "services/iss_service.h"
#include "services/smarthome_service.h"

using namespace PanelDisplay;

WebServer server(80);
Preferences prefs;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

static const uint32_t WEATHER_POLL_MS  = 10UL * 60UL * 1000UL;
static const uint32_t AVIATION_POLL_MS = 15UL * 1000UL;
static const uint32_t ISS_POLL_MS      = 60UL * 1000UL;
static const uint32_t SMARTHOME_POLL_MS = 5UL * 1000UL;
uint32_t lastWeather = 0, lastAviation = 0, lastIss = 0, lastSmartHome = 0;

void handleRoot() {
  server.send(200, "text/plain", "test");
}

void setup() {
  Serial.begin(115200);
  uint32_t serialStart = millis();
  while (!Serial && millis() - serialStart < 3000) {
    delay(20);
  }

  Serial.println("[boot] display begin (TEST BUILD 6)");
  if (!screen.begin()) {
    Serial.println("[boot] display FAILED — halting");
    while (true) delay(1000);
  }
  Serial.println("[boot] display ready");

  prefs.begin("dashboard", false);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  server.on("/", handleRoot);
  server.begin();

  MDNS.begin("dashboard");

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);

  weather_service_update();
  aviation_service_update();
  iss_service_update();
  smarthome_service_update();
}

void loop() {
  server.handleClient();
  if (!mqttClient.connected()) {
    mqttClient.connect("esp32-dashboard-test", MQTT_USER, MQTT_PASSWORD);
  }
  mqttClient.loop();

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

  uint16_t touchX = 0, touchY = 0;
  bool touched = screen.readTouch(&touchX, &touchY);

  uint16_t bg = screen.color565(10, 12, 16);
  uint16_t text = screen.color565(235, 240, 245);
  uint16_t accent = screen.color565(70, 130, 220);

  screen.fillScreen(bg);
  screen.setTextSize(3);
  screen.setTextColor(accent, bg);
  screen.setTextDatum(textdatum_t::top_left);
  screen.drawString("TOUCH TEST 6", 30, 60);

  screen.setTextSize(2);
  screen.setTextColor(text, bg);
  char touchDbg[64];
  if (!screen.touchAvailable()) {
    snprintf(touchDbg, sizeof(touchDbg), "TOUCH: controller NOT initialized");
  } else {
    snprintf(touchDbg, sizeof(touchDbg), "TOUCH: x=%d y=%d count=%d",
             touchX, touchY, screen.lastTouchReadCount());
  }
  screen.drawString(touchDbg, 30, 150);

  char wifiDbg[64];
  snprintf(wifiDbg, sizeof(wifiDbg), "WiFi status: %d", (int)WiFi.status());
  screen.drawString(wifiDbg, 30, 200);

  screen.setTextSize(1);
  screen.setTextColor(screen.color565(90, 100, 110), bg);
  screen.setTextDatum(textdatum_t::top_right);
  screen.drawString(FIRMWARE_VERSION, WIDTH - 6, HEIGHT - 14);

  screen.present();
  delay(50);
}
