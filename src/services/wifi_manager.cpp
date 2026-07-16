#include "wifi_manager.h"
#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

static Preferences prefs;
static WebServer server(80);
static DNSServer dnsServer;
static bool inApMode = false;
static bool mdnsStarted = false;
static const byte DNS_PORT = 53;
static IPAddress apIP(192, 168, 4, 1);

static uint32_t lastReconnectAttempt = 0;
static const uint32_t RECONNECT_INTERVAL_MS = 10000;
static const uint32_t CONNECT_TIMEOUT_MS = 15000;

static String htmlWrap(const String& title, const String& body) {
  String page = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>" + title + "</title><style>";
  page += "body{font-family:sans-serif;background:#0a0c10;color:#ebeef2;padding:24px;max-width:420px;margin:0 auto}";
  page += "h1{font-size:20px}label{display:block;margin-top:16px;font-size:14px;color:#98a2ac}";
  page += "input{width:100%;padding:10px;margin-top:6px;border-radius:6px;border:1px solid #333;background:#1a1d24;color:#fff;box-sizing:border-box}";
  page += "button{margin-top:24px;width:100%;padding:12px;border-radius:6px;border:none;background:#4682dc;color:#fff;font-size:16px}";
  page += "</style></head><body>" + body + "</body></html>";
  return page;
}

static void handlePortalRoot() {
  String body = "<h1>ESP32 Dashboard Setup</h1>";
  body += "<p>Enter your home WiFi details below.</p>";
  body += "<form action='/save' method='POST'>";
  body += "<label>Network name (SSID)</label><input name='ssid' required>";
  body += "<label>Password</label><input name='pass' type='password'>";
  body += "<button type='submit'>Save &amp; Connect</button>";
  body += "</form>";
  server.send(200, "text/html", htmlWrap("Dashboard Setup", body));
}

static void handlePortalNotFound() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

static void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (ssid.length() == 0) {
    server.send(400, "text/html", htmlWrap("Error", "<h1>SSID can't be empty</h1><a href='/'>Back</a>"));
    return;
  }
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);

  String body = "<h1>Saved!</h1><p>Restarting and connecting to " + ssid + "...</p>";
  server.send(200, "text/html", htmlWrap("Saved", body));
  delay(1000);
  ESP.restart();
}

static void startConfigPortal() {
  Serial.println("[WiFi] Starting setup portal (AP mode)");
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP("ESP32-Dashboard-Setup");
  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/", handlePortalRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handlePortalNotFound);
  server.begin();

  inApMode = true;
  Serial.println("[WiFi] Connect to WiFi network 'ESP32-Dashboard-Setup', then open http://192.168.4.1");
}

static void handleStatusRoot() {
  String body = "<h1>ESP32 Dashboard</h1>";
  body += "<p>Connected to: " + WiFi.SSID() + "</p>";
  body += "<p>IP address: " + WiFi.localIP().toString() + "</p>";
  body += "<p>Uptime: " + String(millis() / 1000) + " sec</p>";
  body += "<form action='/reconfigure' method='POST'>";
  body += "<button type='submit'>Reconfigure WiFi</button>";
  body += "</form>";
  server.send(200, "text/html", htmlWrap("Dashboard Status", body));
}

static void handleReconfigure() {
  prefs.remove("ssid");
  prefs.remove("pass");
  String body = "<h1>Restarting into setup mode...</h1><p>Connect to WiFi network 'ESP32-Dashboard-Setup' to reconfigure.</p>";
  server.send(200, "text/html", htmlWrap("Reconfiguring", body));
  delay(1000);
  ESP.restart();
}

static bool connectWithSavedCredentials() {
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");

  if (ssid.length() == 0) {
    ssid = WIFI_SSID;
    pass = WIFI_PASSWORD;
  }
  if (ssid.length() == 0) {
    return false;
  }

  Serial.printf("[WiFi] Connecting to %s...\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void wifi_manager_begin() {
  prefs.begin("dashboard", false);

  if (connectWithSavedCredentials()) {
    inApMode = false;
    Serial.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());

    if (MDNS.begin("dashboard")) {
      mdnsStarted = true;
      Serial.println("[WiFi] mDNS ready: http://dashboard.local");
    }
    server.on("/", handleStatusRoot);
    server.on("/reconfigure", HTTP_POST, handleReconfigure);
    server.begin();
  } else {
    startConfigPortal();
  }
}

void wifi_manager_loop() {
  if (inApMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    return;
  }

  server.handleClient();

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
  return !inApMode && WiFi.status() == WL_CONNECTED;
}

bool wifi_manager_in_setup_mode() {
  return inApMode;
}
