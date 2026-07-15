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

void mqtt_service_loop() {
  if (!mqttClient.connected()) {
    if (mqttClient.connect("esp32-dashboard", MQTT_USER, MQTT_PASSWORD)) {
      mqttClient.subscribe("dashboard/notify");
      mqttClient.subscribe("home/#"); // house-wide status/events, filter in onMessage
    }
    return;
  }
  mqttClient.loop();
}
