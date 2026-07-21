#include "mqtt_service.h"
#include "secrets.h"
#include <WiFi.h>
#include <PubSubClient.h>

static WiFiClient wifiClient;
static PubSubClient mqttClient(wifiClient);

static void onMessage(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  Serial.printf("[MQTT] %s: %s\n", topic, msg.c_str());
  // TODO: hand msg off to screen_manager to render as a toast/notification.
}

void mqtt_service_begin() {
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(onMessage);
}

// If the broker is unreachable/unconfigured, mqttClient.connect() was being
// retried every single networkTask iteration (~10-30ms) with zero backoff --
// hammering the network stack with rapid-fire connection resets whenever the
// broker refused or reset the socket. Now retries are spaced at least 5s
// apart, so a dead/misconfigured broker can no longer spin like that.
static uint32_t lastMqttAttemptMs = 0;
static const uint32_t MQTT_RETRY_INTERVAL_MS = 5000;

void mqtt_service_loop() {
  if (!mqttClient.connected()) {
    uint32_t now = millis();
    if (now - lastMqttAttemptMs < MQTT_RETRY_INTERVAL_MS) {
      return;
    }
    lastMqttAttemptMs = now;
    if (mqttClient.connect("esp32-dashboard", MQTT_USER, MQTT_PASSWORD)) {
      mqttClient.subscribe("dashboard/notify");
      mqttClient.subscribe("home/#"); // house-wide status/events, filter in onMessage
    }
    return;
  }
  mqttClient.loop();
}
