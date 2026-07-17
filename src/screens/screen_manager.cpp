#include "screen_manager.h"
#include "../panel_display.h"
#include "../version.h"
#include "../services/weather_service.h"
#include "../services/smarthome_service.h"
#include "../services/iss_service.h"
#include "../services/aviation_service.h"
#include <math.h>
#include <time.h>
#include <WiFi.h>

using namespace PanelDisplay;

static const char* TAB_NAMES[] = {
  "DASHBOARD", "AVIATION", "PORSCHE", "SMART HOME", "ISS", "WEATHER"
};
static const int TAB_COUNT = sizeof(TAB_NAMES) / sizeof(TAB_NAMES[0]);
static int currentTab = 0;

static uint16_t colorBg;
static uint16_t colorText;
static uint16_t colorDim;
static uint16_t colorAccent;

static bool touchWasDown = false;
static uint32_t touchDownMs = 0;
static const uint32_t TAP_MIN_MS = 50;
static const uint32_t TAP_MAX_MS = 600;

static void drawHeader() {
  screen.fillRect(0, 0, WIDTH, 40, colorAccent);
  screen.setTextSize(2);
  screen.setTextColor(colorBg, colorAccent);
  screen.setTextDatum(textdatum_t::top_left);
  screen.drawString(TAB_NAMES[currentTab], 10, 12);

  screen.setTextSize(1);
  screen.setTextColor(colorBg, colorAccent);
  screen.setTextDatum(textdatum_t::top_right);
  char tabIndicator[16];
  snprintf(tabIndicator, sizeof(tabIndicator), "%d/%d  TAP>", currentTab + 1, TAB_COUNT);
  screen.drawString(tabIndicator, WIDTH - 10, 15);
}

static void draw_dashboard() {
  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);
  screen.setTextDatum(textdatum_t::top_left);

  int y = 60;
  if (WiFi.status() == WL_CONNECTED) {
    char line[64];
    snprintf(line, sizeof(line), "WiFi: %s (%s)", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    screen.setTextColor(screen.color565(80, 200, 120), colorBg);
    screen.drawString(line, 20, y);
  } else {
    screen.setTextColor(screen.color565(220, 80, 80), colorBg);
    char line[32];
    snprintf(line, sizeof(line), "WiFi: disconnected (%d)", (int)WiFi.status());
    screen.drawString(line, 20, y);
  }
  screen.setTextColor(colorText, colorBg);
  y += 40;

  if (g_weather.valid) {
    char line[64];
    snprintf(line, sizeof(line), "Weather: %.0fF  %s", g_weather.tempF, g_weather.condition.c_str());
    screen.drawString(line, 20, y);
  } else {
    screen.drawString("Weather: --", 20, y);
  }
  y += 40;

  if (g_iss.valid) {
    char line[64];
    snprintf(line, sizeof(line), "ISS altitude: %.0f km", g_iss.altitudeKm);
    screen.drawString(line, 20, y);
  } else {
    screen.drawString("ISS: --", 20, y);
  }
  y += 40;

  char line[64];
  snprintf(line, sizeof(line), "House: %d devices online", g_deviceCount);
  screen.drawString(line, 20, y);
  y += 40;

  snprintf(line, sizeof(line), "Aircraft nearby: %d", g_aircraftCount);
  screen.drawString(line, 20, y);
}

static const int RADAR_CX = 240;
static const int RADAR_CY = 260;
static const int RADAR_RADIUS = 190;
static const float RADAR_MAX_RANGE_NM = 40.0f;
static const int RADAR_RINGS = 4;

static void draw_aviation() {
  uint16_t colorGrid = screen.color565(40, 60, 55);
  uint16_t colorPlane = screen.color565(255, 70, 90);
  uint16_t colorLabel = screen.color565(200, 220, 210);

  for (int i = 1; i <= RADAR_RINGS; i++) {
    int r = RADAR_RADIUS * i / RADAR_RINGS;
    screen.drawCircle(RADAR_CX, RADAR_CY, r, colorGrid);
  }
  screen.drawLine(RADAR_CX - RADAR_RADIUS, RADAR_CY, RADAR_CX + RADAR_RADIUS, RADAR_CY, colorGrid);
  screen.drawLine(RADAR_CX, RADAR_CY - RADAR_RADIUS, RADAR_CX, RADAR_CY + RADAR_RADIUS, colorGrid);

  screen.setTextSize(1);
  screen.setTextColor(colorLabel, colorBg);
  screen.setTextDatum(textdatum_t::middle_center);
  screen.drawString("N", RADAR_CX, RADAR_CY - RADAR_RADIUS - 10);
  screen.drawString("S", RADAR_CX, RADAR_CY + RADAR_RADIUS + 10);
  screen.drawString("E", RADAR_CX + RADAR_RADIUS + 12, RADAR_CY);
  screen.drawString("W", RADAR_CX - RADAR_RADIUS - 12, RADAR_CY);

  for (int i = 0; i < g_aircraftCount; i++) {
    Aircraft& a = g_aircraft[i];
    float rangeFrac = a.distanceNm / RADAR_MAX_RANGE_NM;
    if (rangeFrac > 1.0f) continue;

    float bearingRad = a.bearingFromHome * (PI / 180.0f);
    int px = RADAR_CX + (int)(sinf(bearingRad) * rangeFrac * RADAR_RADIUS);
    int py = RADAR_CY - (int)(cosf(bearingRad) * rangeFrac * RADAR_RADIUS);

    screen.fillCircle(px, py, 4, colorPlane);

    screen.setTextColor(colorLabel, colorBg);
    screen.setTextDatum(textdatum_t::top_left);
    const char* label = a.callsign.length() > 0 ? a.callsign.c_str() : "????";
    screen.drawString(label, px + 8, py - 6);
  }

  int listX = 470;
  int listY = 55;
  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);
  screen.setTextDatum(textdatum_t::top_left);
  char header[32];
  snprintf(header, sizeof(header), "NEARBY (%d)", g_aircraftCount);
  screen.drawString(header, listX, listY);
  listY += 36;

  screen.setTextSize(1);
  int shown = 0;
  for (int i = 0; i < g_aircraftCount && shown < 8; i++) {
    Aircraft& a = g_aircraft[i];
    char row[64];
    const char* callsign = a.callsign.length() > 0 ? a.callsign.c_str() : "????";
    snprintf(row, sizeof(row), "%-8s %5dft  %.0fnm", callsign, a.altitudeFt, a.distanceNm);
    screen.setTextColor(colorText, colorBg);
    screen.drawString(row, listX, listY);
    listY += 22;
    shown++;
  }
  if (g_aircraftCount == 0) {
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("No aircraft in range", listX, listY);
  }
}

static void draw_smarthome() {
  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);
  screen.setTextDatum(textdatum_t::top_left);
  char header[32];
  snprintf(header, sizeof(header), "%d DEVICES ONLINE", g_deviceCount);
  screen.drawString(header, 20, 55);

  if (g_deviceCount == 0) {
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("No devices found — check Hubitat/HA connection", 20, 100);
    return;
  }

  const int colWidth = 380;
  const int rowHeight = 90;
  const int rowsPerCol = 4;
  const int startX = 20;
  const int startY = 100;

  for (int i = 0; i < g_deviceCount && i < rowsPerCol * 2; i++) {
    int col = i / rowsPerCol;
    int row = i % rowsPerCol;
    int x = startX + col * colWidth;
    int y = startY + row * rowHeight;

    bool isOn = g_devices[i].state.equalsIgnoreCase("on") ||
                g_devices[i].state.equalsIgnoreCase("locked") ||
                g_devices[i].state.equalsIgnoreCase("closed");
    uint16_t stateColor = isOn ? screen.color565(80, 200, 120) : colorDim;

    screen.fillRect(x, y, colWidth - 20, rowHeight - 12, screen.color565(25, 28, 34));

    screen.setTextSize(2);
    screen.setTextColor(colorText, screen.color565(25, 28, 34));
    screen.setTextDatum(textdatum_t::top_left);
    const char* name = g_devices[i].name.length() > 0 ? g_devices[i].name.c_str() : "(unnamed)";
    screen.drawString(name, x + 12, y + 10);

    screen.setTextSize(1);
    screen.setTextColor(colorDim, screen.color565(25, 28, 34));
    screen.drawString(g_devices[i].type.c_str(), x + 12, y + 38);

    screen.setTextSize(2);
    screen.setTextColor(stateColor, screen.color565(25, 28, 34));
    screen.drawString(g_devices[i].state.c_str(), x + 12, y + 54);
  }

  if (g_deviceCount > rowsPerCol * 2) {
    screen.setTextSize(1);
    screen.setTextColor(colorDim, colorBg);
    char more[32];
    snprintf(more, sizeof(more), "+ %d more not shown", g_deviceCount - rowsPerCol * 2);
    screen.drawString(more, startX, startY + rowsPerCol * rowHeight + 10);
  }
}

static void draw_weather() {
  screen.setTextDatum(textdatum_t::top_left);

  if (!g_weather.valid) {
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("No weather data yet", 20, 100);
    return;
  }

  screen.setTextSize(4);
  screen.setTextColor(colorText, colorBg);
  char tempStr[16];
  snprintf(tempStr, sizeof(tempStr), "%.0fF", g_weather.tempF);
  screen.drawString(tempStr, 30, 70);

  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString(g_weather.condition.c_str(), 30, 130);

  int y = 190;
  screen.setTextSize(2);
  char row[48];

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Feels like", 30, y);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%.0fF", g_weather.feelsLikeF);
  screen.drawString(row, 260, y);
  y += 44;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Wind", 30, y);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%.0f mph", g_weather.windMph);
  screen.drawString(row, 260, y);
  y += 44;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Humidity", 30, y);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%d%%", g_weather.humidity);
  screen.drawString(row, 260, y);
  y += 44;

  screen.setTextSize(1);
  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Hourly / 7-day forecast not wired up yet", 30, y + 20);
}

static String formatUnixTime(uint32_t unixTime) {
  if (unixTime == 0) return "unknown";
  time_t t = (time_t)unixTime;
  struct tm* timeInfo = localtime(&t);
  char buf[32];
  strftime(buf, sizeof(buf), "%a %I:%M %p", timeInfo);
  return String(buf);
}

static void draw_iss() {
  screen.setTextDatum(textdatum_t::top_left);

  if (!g_iss.valid) {
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("No ISS data yet", 20, 100);
    screen.setTextSize(1);
    char errLine[48];
    snprintf(errLine, sizeof(errLine), "Last HTTP result: %d", g_iss.lastHttpCode);
    screen.drawString(errLine, 20, 140);
    screen.drawString("(200=ok, 401/403=bad API key, negative=connection error)", 20, 165);
    return;
  }

  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);
  screen.drawString("Current Position", 30, 60);

  int y = 100;
  char row[64];
  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Latitude", 30, y);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%.2f", g_iss.lat);
  screen.drawString(row, 260, y);
  y += 40;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Longitude", 30, y);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%.2f", g_iss.lon);
  screen.drawString(row, 260, y);
  y += 40;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Altitude", 30, y);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%.0f km", g_iss.altitudeKm);
  screen.drawString(row, 260, y);
  y += 60;

  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString("Next Visible Pass", 30, y);
  y += 40;

  if (g_iss.nextPassUnix > 0) {
    screen.setTextColor(colorText, colorBg);
    String passTime = formatUnixTime(g_iss.nextPassUnix);
    screen.drawString(passTime, 30, y);
    y += 40;

    screen.setTextColor(colorDim, colorBg);
    screen.drawString("Duration", 30, y);
    screen.setTextColor(colorText, colorBg);
    snprintf(row, sizeof(row), "%d min", g_iss.nextPassDurationSec / 60);
    screen.drawString(row, 260, y);

    uint32_t nowUnix = (uint32_t)time(nullptr);
    if (nowUnix >= g_iss.nextPassUnix &&
        nowUnix <= g_iss.nextPassUnix + (uint32_t)g_iss.nextPassDurationSec) {
      y += 50;
      screen.setTextSize(3);
      screen.setTextColor(screen.color565(80, 220, 120), colorBg);
      screen.drawString("VISIBLE NOW - LOOK UP!", 30, y);
    }
  } else {
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("No upcoming pass found", 30, y);
  }
}

static void draw_placeholder(const char* label) {
  screen.setTextSize(2);
  screen.setTextColor(colorDim, colorBg);
  screen.setTextDatum(textdatum_t::middle_center);
  char msg[64];
  snprintf(msg, sizeof(msg), "%s\n(screen not built yet)", label);
  screen.drawString(msg, WIDTH / 2, HEIGHT / 2);
}

void screen_manager_init() {
  colorBg = screen.color565(10, 12, 16);
  colorText = screen.color565(235, 240, 245);
  colorDim = screen.color565(120, 130, 140);
  colorAccent = screen.color565(70, 130, 220);
}

void screen_manager_draw() {
  screen.fillScreen(colorBg);
  drawHeader();

  switch (currentTab) {
    case 0: draw_dashboard(); break;
    case 1: draw_aviation(); break;
    case 2: draw_placeholder("Porsche"); break;
    case 3: draw_smarthome(); break;
    case 4: draw_iss(); break;
    case 5: draw_weather(); break;
  }

  screen.setTextSize(1);
  screen.setTextColor(colorDim, colorBg);
  screen.setTextDatum(textdatum_t::top_right);
  screen.drawString(FIRMWARE_VERSION, WIDTH - 6, HEIGHT - 14);
}

void screen_manager_handle_touch(bool touched, uint16_t x, uint16_t y) {
  uint32_t now = millis();
  if (touched && !touchWasDown) {
    touchDownMs = now;
  }
  if (!touched && touchWasDown) {
    uint32_t held = now - touchDownMs;
    if (held >= TAP_MIN_MS && held <= TAP_MAX_MS) {
      currentTab = (currentTab + 1) % TAB_COUNT;
    }
  }
  touchWasDown = touched;
}
