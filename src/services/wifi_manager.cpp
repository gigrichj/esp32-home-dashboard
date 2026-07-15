#include "wifi_manager.h"
#include <WiFi.h>

static uint32_t lastReconnectAttempt = 0;
static const uint32_t RECONNECT_INTERVAL_MS = 10000;

void wifi_manager_begin(const char* ssid, const char* password) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.printf("[WiFi] Connecting to %s...\n", ssid);
}

void wifi_manager_loop() {
  if (WiFi.status() != WL_CONNECTED) {
    uint32_t now = millis();
    if (now - lastReconnectAttempt > RECONNECT_INTERVAL_MS) {
      lastReconnectAttempt = now;
      Serial.println("[WiFi] Reconnecting...");
      WiFi.reconnect();
    }
  }
}

bool wifi_manager_is_connected() {
  return WiFi.status() == WL_CONNECTED;
}
