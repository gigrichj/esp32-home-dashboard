#include "smarthome_service.h"
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>

DeviceState g_devices[MAX_DEVICES];
int g_deviceCount = 0;

static void pollHubitat() {
  HTTPClient http;
  char url[200];
  snprintf(url, sizeof(url),
    "http://%s/apps/api/%s/devices/all?access_token=%s",
    HUBITAT_HUB_IP, HUBITAT_APP_ID, HUBITAT_ACCESS_TOKEN);

  http.begin(url);
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getStream())) {
      for (JsonObject d : doc.as<JsonArray>()) {
        if (g_deviceCount >= MAX_DEVICES) break;
        DeviceState& dev = g_devices[g_deviceCount++];
        dev.name  = d["label"].as<String>();
        dev.type  = d["type"].as<String>();
        // Hubitat attributes vary by capability; "switch" is common for lights.
        JsonArray attrs = d["attributes"].as<JsonArray>();
        for (JsonObject a : attrs) {
          if (a["name"] == "switch") dev.state = a["currentValue"].as<String>();
        }
      }
    }
  } else {
    Serial.printf("[Hubitat] HTTP %d\n", code);
  }
  http.end();
}

static void pollHomeAssistant() {
  HTTPClient http;
  char url[200];
  snprintf(url, sizeof(url), "%s/api/states", HA_BASE_URL);

  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + HA_LONG_LIVED_TOKEN);
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getStream())) {
      for (JsonObject e : doc.as<JsonArray>()) {
        if (g_deviceCount >= MAX_DEVICES) break;
        String entityId = e["entity_id"].as<String>();
        // Filter to the domains the dashboard cares about.
        if (entityId.startsWith("light.") || entityId.startsWith("lock.") ||
            entityId.startsWith("cover.") || entityId.startsWith("climate.")) {
          DeviceState& dev = g_devices[g_deviceCount++];
          dev.name  = e["attributes"]["friendly_name"].as<String>();
          dev.type  = entityId.substring(0, entityId.indexOf('.'));
          dev.state = e["state"].as<String>();
        }
      }
    }
  } else {
    Serial.printf("[HomeAssistant] HTTP %d\n", code);
  }
  http.end();
}

void smarthome_service_update() {
  if (!wifi_manager_is_connected()) return;
  g_deviceCount = 0;
  pollHubitat();
  pollHomeAssistant();
}

bool hubitat_send_command(const String& deviceId, const String& command) {
  if (!wifi_manager_is_connected()) return false;
  HTTPClient http;
  char url[220];
  snprintf(url, sizeof(url),
    "http://%s/apps/api/%s/devices/%s/%s?access_token=%s",
    HUBITAT_HUB_IP, HUBITAT_APP_ID, deviceId.c_str(), command.c_str(), HUBITAT_ACCESS_TOKEN);
  http.begin(url);
  int code = http.GET();
  http.end();
  return code == 200;
}

bool ha_call_service(const String& domain, const String& service, const String& entityId) {
  if (!wifi_manager_is_connected()) return false;
  HTTPClient http;
  char url[200];
  snprintf(url, sizeof(url), "%s/api/services/%s/%s", HA_BASE_URL, domain.c_str(), service.c_str());
  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + HA_LONG_LIVED_TOKEN);
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"entity_id\":\"") + entityId + "\"}";
  int code = http.POST(body);
  http.end();
  return code == 200;
}
