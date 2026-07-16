#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"
#include "panel_display.h"
#include "version.h"

using namespace PanelDisplay;

void setup() {
  Serial.begin(115200);
  uint32_t serialStart = millis();
  while (!Serial && millis() - serialStart < 3000) {
    delay(20);
  }

  Serial.println("[boot] display begin (WIFI-RADIO-ONLY TEST BUILD)");
  if (!screen.begin()) {
    Serial.println("[boot] display FAILED — halting");
    while (true) delay(1000);
  }
  Serial.println("[boot] display ready");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void loop() {
  uint16_t touchX = 0, touchY = 0;
  bool touched = screen.readTouch(&touchX, &touchY);

  uint16_t bg = screen.color565(10, 12, 16);
  uint16_t text = screen.color565(235, 240, 245);
  uint16_t accent = screen.color565(70, 130, 220);

  screen.fillScreen(bg);
  screen.setTextSize(3);
  screen.setTextColor(accent, bg);
  screen.setTextDatum(textdatum_t::top_left);
  screen.drawString("TOUCH TEST (WiFi radio only)", 30, 60);

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
