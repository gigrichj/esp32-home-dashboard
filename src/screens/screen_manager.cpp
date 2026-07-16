#include "screen_manager.h"
#include "../panel_display.h"
#include "../services/weather_service.h"
#include "../services/smarthome_service.h"
#include "../services/iss_service.h"
#include "../services/aviation_service.h"

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

// Touch-tap tracking, mirroring the confirmed-working reference project's
// pattern: a short press-then-release cycles to the next tab.
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
    case 1: draw_placeholder("Aviation / Radar"); break;
    case 2: draw_placeholder("Porsche"); break;
    case 3: draw_placeholder("Smart Home"); break;
    case 4: draw_placeholder("ISS Tracker"); break;
    case 5: draw_placeholder("Weather Detail"); break;
  }
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
