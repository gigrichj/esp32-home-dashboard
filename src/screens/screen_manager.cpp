#include "screen_manager.h"
#include <esp_heap_caps.h>
#include "../panel_display.h"
#include "../version.h"
#include "../services/weather_service.h"
#include "../services/iss_service.h"
#include "../services/aviation_service.h"
#include "../services/air_quality_service.h"
#include "../services/astro_seeing_service.h"
#include "../services/trend_history_service.h"
#include "secrets.h"
#include "../debug_log.h"
#include "../debug_controls.h"
#include <math.h>
#include <time.h>
#include <WiFi.h>

using namespace PanelDisplay;

static const char* TAB_NAMES[] = {
  "DASHBOARD", "AVIATION", "ASTRO", "ISS", "WEATHER", "DEBUG", "TRENDS"
};
static const int TAB_COUNT = sizeof(TAB_NAMES) / sizeof(TAB_NAMES[0]);

// Colors aircraft by altitude band, the way flight-tracking apps shade
// low/GA traffic differently from high-altitude airliners.
static uint16_t colorForAltitude(int altFt) {
  if (altFt < 5000)  return screen.color565(255, 210, 60);   // low / GA - yellow
  if (altFt < 15000) return screen.color565(90, 200, 255);   // climbing - cyan
  if (altFt < 30000) return screen.color565(120, 220, 120);  // cruise - green
  return screen.color565(255, 140, 60);                      // high altitude - orange
}

// Dot size grows with altitude too, alongside the color, so low/high
// traffic is distinguishable even at a glance.
static int dotRadiusForAltitude(int altFt) {
  if (altFt < 5000)  return 3;
  if (altFt < 15000) return 4;
  if (altFt < 30000) return 5;
  return 6;
}

// 7500 = hijack, 7600 = radio failure, 7700 = general emergency.
static bool isEmergencySquawk(const String& squawk) {
  return squawk == "7500" || squawk == "7600" || squawk == "7700";
}

// Colors an OpenWeatherMap AQI index (1=Good .. 5=Very Poor) green-to-red.
static uint16_t airQualityColor(int aqi) {
  switch (aqi) {
    case 1: return screen.color565(80, 200, 120);
    case 2: return screen.color565(160, 200, 60);
    case 3: return screen.color565(230, 200, 40);
    case 4: return screen.color565(230, 130, 40);
    case 5: return screen.color565(220, 60, 60);
    default: return screen.color565(120, 130, 140);
  }
}

// Interpolates across a list of RGB stops, evenly spaced across frac 0..1.
// Used to draw continuous gradient bars (rather than flat segments) for
// both the AQI and UV Index readouts below.
static uint16_t multiStopGradient(float frac, const uint8_t stops[][3], int stopCount) {
  frac = constrain(frac, 0.0f, 1.0f);
  float segF = frac * (float)(stopCount - 1);
  int seg = (int)segF;
  if (seg >= stopCount - 1) seg = stopCount - 2;
  float localT = segF - (float)seg;
  uint8_t r = stops[seg][0] + (uint8_t)(((int)stops[seg + 1][0] - (int)stops[seg][0]) * localT);
  uint8_t g = stops[seg][1] + (uint8_t)(((int)stops[seg + 1][1] - (int)stops[seg][1]) * localT);
  uint8_t b = stops[seg][2] + (uint8_t)(((int)stops[seg + 1][2] - (int)stops[seg][2]) * localT);
  return screen.color565(r, g, b);
}

// Same 5 colors as airQualityColor() above, but continuous rather than
// stepped, so the gradient bar reads as a smooth scale with a pointer
// marking the exact value instead of N-of-5 lit boxes.
static uint16_t aqiGradientColor(float frac) {
  static const uint8_t stops[5][3] = {
    {80, 200, 120}, {160, 200, 60}, {230, 200, 40}, {230, 130, 40}, {220, 60, 60}
  };
  return multiStopGradient(frac, stops, 5);
}

// Standard UV Index color scale: Low(0-2)/Moderate(3-5)/High(6-7)/
// Very High(8-10)/Extreme(11+), same green-through-purple convention
// used by most weather services.
static uint16_t uvGradientColor(float frac) {
  static const uint8_t stops[5][3] = {
    {80, 200, 120}, {230, 200, 40}, {230, 130, 40}, {220, 60, 60}, {150, 80, 220}
  };
  return multiStopGradient(frac, stops, 5);
}

static uint16_t uvIndexColor(float uv) {
  if (uv < 3)  return screen.color565(80, 200, 120);
  if (uv < 6)  return screen.color565(230, 200, 40);
  if (uv < 8)  return screen.color565(230, 130, 40);
  if (uv < 11) return screen.color565(220, 60, 60);
  return screen.color565(150, 80, 220);
}

static const char* uvIndexLabel(float uv) {
  if (uv < 3)  return "Low";
  if (uv < 6)  return "Moderate";
  if (uv < 8)  return "High";
  if (uv < 11) return "Very High";
  return "Extreme";
}

static int currentTab = 0;

static uint16_t colorBg;
static uint16_t colorSuccess;
static uint16_t colorDanger;
static uint16_t colorText;
static uint16_t colorDim;
static uint16_t colorAccent;

// Day (normal) and Night (red-shifted, vision-preserving) palettes.
// screen_manager_draw() picks between these every frame and assigns the
// active set to the colorBg/colorText/etc. statics above, so every
// existing draw_* function keeps using those same names unchanged.
static uint16_t colorBgDay, colorSuccessDay, colorDangerDay, colorTextDay, colorDimDay, colorAccentDay;
static uint16_t colorBgNight, colorSuccessNight, colorDangerNight, colorTextNight, colorDimNight, colorAccentNight;

static bool touchWasDown = false;
static uint32_t touchDownMs = 0;
static const uint32_t TAP_MIN_MS = 50;
static const uint32_t TAP_MAX_MS = 600;
static const uint32_t LONGPRESS_MIN_MS = 900;   // hold longer than this toggles night mode
static uint16_t touchDownX = 0;
static uint16_t touchDownY = 0;
static uint16_t touchMinX = 0;                  // smallest X seen so far this gesture (left excursion)
static uint16_t touchMaxX = 0;                  // largest X seen so far this gesture (right excursion)

static bool g_pageLocked = false;               // when true, navigation (swipe, tap-to-advance,
                                                 // idle auto-cycle) is suppressed -- the current
                                                 // page keeps drawing and updating live data
                                                 // normally, it just won't change tabs until
                                                 // toggled again with a top-to-bottom swipe.
static uint16_t touchMinY = 0;                  // smallest Y seen so far this gesture (upward excursion)
static uint16_t touchMaxY = 0;                  // largest Y seen so far this gesture (downward excursion)
static const int VERTICAL_SWIPE_MIN_PX = 40;    // minimum downward excursion to count as a
                                                 // top-to-bottom swipe, same peak-excursion approach
                                                 // used for the horizontal swipe (see SWIPE_MIN_PX).
static const int SWIPE_MIN_PX = 40;             // minimum excursion (in either direction from the
                                                 // touch-down point) to count as a swipe. Measured as
                                                 // peak excursion during the gesture rather than net
                                                 // start-to-release displacement, since DRAW_INTERVAL_MS
                                                 // only samples touch once per ~200ms frame -- a quick
                                                 // swipe can be under-sampled at both ends, shrinking
                                                 // the naive "last - down" distance even when the real
                                                 // physical swipe was well past threshold. Peak excursion
                                                 // is symmetric and doesn't favor either direction.

static uint32_t lastInteractionMs = 0;
static uint32_t lastAutoAdvanceMs = 0;
static const uint32_t IDLE_TIMEOUT_MS = 30000;        // no touch for this long -> start auto-cycling pages
static const uint32_t AUTO_CYCLE_INTERVAL_MS = 15000; // page advance cadence once idle

static bool g_nightModeOn = false; // night (red-shifted) mode only ever changes via long-press --
                                    // no automatic sunset/sunrise trigger.

// Alert takeover banner -- overlays the current page (rather than being
// its own tab) when a real threshold trips: tonight's astro verdict
// swings to GOOD, storm risk crosses into High Risk, or an emergency
// squawk appears among nearby aircraft. Fires once at the MOMENT of
// change (comparing against last frame's state), not continuously while
// the condition remains true, so it doesn't nag on every redraw.
static bool g_alertActive = false;
static uint32_t g_alertShownAtMs = 0;
static const uint32_t ALERT_DURATION_MS = 15000; // stays up 15s, or until tapped
static char g_alertMessage[64] = "";
static uint16_t g_alertColorOverride = 0; // set from colorSuccess/colorDanger at trigger time

static bool g_prevAstroWasGood = false;
static bool g_prevStormWasHigh = false;
static bool g_prevAnyEmergency = false;
static bool g_alertStatePrimed = false; // avoids firing a false alert on the very first frame,
                                        // before we have a real "previous" state to compare against

static String formatCurrentDateTime() {
  time_t now = time(nullptr);
  if (now < 100000) return String("Time syncing...");
  struct tm* t = localtime(&now);
  char buf[40];
  strftime(buf, sizeof(buf), "%a, %b %d  %I:%M %p", t);
  return String(buf);
}

static const int ISS_TAB_INDEX = 3;

static void drawHeader() {
  screen.fillRect(0, 0, WIDTH, 40, colorAccent);
  screen.setTextSize(2);
  screen.setTextColor(colorBg, colorAccent);
  screen.setTextDatum(textdatum_t::top_left);
  if (currentTab == 0) {
    screen.drawString("DASHBOARD - LORTON,VA", 10, 12);
  } else if (currentTab == ISS_TAB_INDEX) {
    // Home coordinates alongside the title, same pattern as the
    // Dashboard's "- LORTON,VA" suffix -- useful reference since this
    // page is all about position relative to home.
    char issTitle[56];
    snprintf(issTitle, sizeof(issTitle), "ISS - LORTON,VA - %.4f, %.4f", (double)HOME_LAT, (double)HOME_LON);
    screen.drawString(issTitle, 10, 12);
  } else {
    screen.drawString(TAB_NAMES[currentTab], 10, 12);
  }

  screen.setTextSize(1);
  screen.setTextColor(colorBg, colorAccent);
  screen.setTextDatum(textdatum_t::top_right);
  char tabIndicator[16];
  snprintf(tabIndicator, sizeof(tabIndicator), "%d/%d  TAP>", currentTab + 1, TAB_COUNT);
  screen.drawString(tabIndicator, WIDTH - 10, 15);
}

static void drawCloudIcon(int cx, int cy, int r, uint16_t color); // defined further down
static void drawWeatherBackground(int weatherId, bool isNight); // defined further down, used here

static void drawDashboardBackground() {
  uint32_t t = millis();

  // Full weather-reactive background (rain, snow, sun/moon, thunderstorm
  // flashes, clouds) -- the same effects used on the Weather page, so the
  // Dashboard reflects current conditions instead of a generic low-key
  // cloud drift. isNight computed the same way draw_weather() does.
  if (g_weather.valid) {
    bool isNight = false;
    time_t nowTime = time(nullptr);
    if (nowTime > 100000 && g_weather.sunriseUnix > 0 && g_weather.sunsetUnix > 0) {
      isNight = (uint32_t)nowTime < g_weather.sunriseUnix || (uint32_t)nowTime > g_weather.sunsetUnix;
    }
    drawWeatherBackground(g_weather.weatherId, isNight);
  }

  // A little airplane silhouette continuously crossing the lower part of
  // the screen - a fun nod to the fact this thing tracks real aircraft.
  {
    // A proper top-down plane silhouette -- fuselage, nose cone, swept
    // wings, and small tail fins -- rather than a couple of plain
    // triangles. Brightened a bit too, so the shape actually reads
    // against the dark background instead of nearly disappearing.
    uint16_t planeColor = screen.color565(95, 110, 135);
    int span = WIDTH + 80;
    int x = (int)((t / 18) % (uint32_t)span) - 40;
    int y = 410;

    // Fuselage + nose cone, pointing right (direction of travel).
    screen.fillRect(x - 18, y - 2, 24, 4, planeColor);
    screen.fillTriangle(x + 6, y - 4, x + 6, y + 4, x + 16, y, planeColor);

    // Swept wings, positioned just behind the nose.
    screen.fillTriangle(x + 2, y, x - 8, y - 14, x - 2, y, planeColor);
    screen.fillTriangle(x + 2, y, x - 8, y + 14, x - 2, y, planeColor);

    // Small tail fins near the back.
    screen.fillTriangle(x - 14, y, x - 20, y - 7, x - 16, y, planeColor);
    screen.fillTriangle(x - 14, y, x - 20, y + 7, x - 16, y, planeColor);
  }

  // An ISS icon drifting the opposite direction, higher up, echoing the
  // ISS page's icon without needing real orbital data for this ambient
  // touch -- just a bit of life on the dashboard.
  {
    uint16_t issColor = screen.color565(120, 100, 160);
    int span = WIDTH + 80;
    int x = span - 40 - (int)((t / 26) % (uint32_t)span);
    int y = 160;
    screen.fillRect(x - 3, y - 3, 6, 6, issColor);
    screen.fillRect(x - 15, y - 2, 9, 4, issColor);
    screen.fillRect(x + 6, y - 2, 9, 4, issColor);
  }

  // A small floating galaxy -- a rotating spiral of dots around a bright
  // core -- drifting slowly across the upper part of the screen. Purely
  // decorative, but a nice nod to the astro page.
  {
    uint16_t galaxyCore = screen.color565(215, 195, 255);
    uint16_t galaxyArm = screen.color565(120, 100, 170);
    int span = WIDTH + 100;
    int gx = (int)((t / 55) % (uint32_t)span) - 50;
    int gy = 120;

    screen.fillCircle(gx, gy, 4, galaxyCore);

    float rotation = (float)(t / 30) * (PI / 180.0f);
    for (int arm = 0; arm < 2; arm++) {
      float armOffset = arm * PI;
      for (int d = 1; d <= 4; d++) {
        float angle = rotation + armOffset + d * 0.6f;
        float radius = d * 5.0f;
        int dx = gx + (int)(cosf(angle) * radius);
        int dy = gy + (int)(sinf(angle) * radius * 0.5f);
        screen.fillCircle(dx, dy, 2, galaxyArm);
      }
    }
  }
}

static int countVisibleAircraft(); // defined further down, used in draw_dashboard()
static int findTonightAstroIndex(); // defined further down, used in draw_dashboard()
static uint16_t astroSeverityColor(int idx, int maxIdx); // defined further down, used in draw_dashboard()

// Draws a temperature as "NN" + a hand-drawn degree ring + "F", since the
// custom bitmap font has no degree glyph (same trick as the hand-drawn
// percent glyph used for humidity). Returns the x position just past the
// "F", so callers can chain more text after it (e.g. a condition string).
static int drawTempF(float tempF, int x, int y, int textSize, uint16_t fgColor, uint16_t bgColor) {
  char numStr[12];
  snprintf(numStr, sizeof(numStr), "%.0f", tempF);
  screen.setTextSize(textSize);
  screen.setTextColor(fgColor, bgColor);
  screen.drawString(numStr, x, y);
  int numWidth = screen.textWidth(numStr);

  int ringR = textSize * 2;
  int gap = textSize * 2;
  int ringCx = x + numWidth + gap + ringR;
  int ringCy = y + ringR;
  screen.drawCircle(ringCx, ringCy, ringR, fgColor);

  int fX = ringCx + ringR + (textSize * 2);
  screen.drawString("F", fX, y);
  return fX + screen.textWidth("F");
}

static void draw_dashboard() {
  drawDashboardBackground();
  astro_recompute_moon_phase(); // keeps moon illum% current for the ASTRO column below

  screen.setTextSize(3);
  screen.setTextColor(colorAccent, colorBg);
  screen.setTextDatum(textdatum_t::top_left);
  screen.drawString(formatCurrentDateTime(), 20, 55);
  screen.setTextColor(colorText, colorBg);

  // WiFi status - small, tucked in the top-right corner instead of the
  // main list, so it doesn't compete for attention with the real data.
  // A small signal-strength bar icon sits just to the left of the text,
  // giving an at-a-glance read without needing to parse the dBm number.
  screen.setTextSize(1);
  screen.setTextDatum(textdatum_t::top_right);
  if (WiFi.status() == WL_CONNECTED) {
    char line[64];
    snprintf(line, sizeof(line), "WiFi: %s (%s)", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    screen.setTextColor(colorSuccess, colorBg);
    screen.drawString(line, WIDTH - 10, 50);

    {
      // 4-bar signal icon, classic ascending-height bars. RSSI thresholds
      // roughly follow common phone/router conventions: > -55 excellent,
      // > -65 good, > -75 fair, otherwise weak (still shows 1 bar so it
      // doesn't look broken/blank at low-but-connected signal).
      int rssi = WiFi.RSSI();
      int litBars = (rssi > -55) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : 1;

      int lineWidth = (int)strlen(line) * 6; // ~6px/char at text size 1
      int iconBarW = 4, iconGap = 2;
      int iconTotalW = iconBarW * 4 + iconGap * 3;
      int iconRight = WIDTH - 10 - lineWidth - 10; // 10px gap before the text
      int iconBaseY = 58; // bars grow upward from this baseline

      for (int b = 0; b < 4; b++) {
        int barH = 3 + b * 3; // ascending heights: 3,6,9,12px
        int bx = iconRight - iconTotalW + b * (iconBarW + iconGap);
        uint16_t barColor = (b < litBars) ? colorSuccess : colorDim;
        screen.fillRect(bx, iconBaseY - barH, iconBarW, barH, barColor);
      }
    }
  } else {
    char line[32];
    snprintf(line, sizeof(line), "WiFi: disconnected (%d)", (int)WiFi.status());
    screen.setTextColor(colorDanger, colorBg);
    screen.drawString(line, WIDTH - 10, 50);
  }
  screen.setTextDatum(textdatum_t::top_left);
  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);

  int leftX = 20;
  int rightX = 420;
  char line[64];

  // WEATHER + AIR QUALITY column
  int y = 207;
  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString("WEATHER", leftX, y);
  screen.drawLine(leftX, y + 20, leftX + 84, y + 20, colorAccent);
  y += 30;

  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);
  if (g_weather.valid) {
    int afterF = drawTempF(g_weather.tempF, leftX, y, 2, colorText, colorBg);
    char condLine[48];
    snprintf(condLine, sizeof(condLine), "  %s", g_weather.condition.c_str());
    screen.setTextColor(colorText, colorBg);
    screen.drawString(condLine, afterF, y);
  } else {
    screen.drawString("--", leftX, y);
  }
  y += 34;

  if (g_airQuality.valid) {
    char aqLine[64];
    snprintf(aqLine, sizeof(aqLine), "Air quality: %s (AQI %d)", air_quality_label(g_airQuality.aqi), g_airQuality.aqi);
    screen.setTextColor(airQualityColor(g_airQuality.aqi), colorBg);
    screen.drawString(aqLine, leftX, y);
    screen.setTextColor(colorText, colorBg);
  } else {
    screen.drawString("Air quality: --", leftX, y);
  }
  y += 34;

  if (g_weather.uvValid) {
    char uvLine[64];
    snprintf(uvLine, sizeof(uvLine), "UV Index: %s (%.0f)", uvIndexLabel(g_weather.uvIndex), g_weather.uvIndex);
    screen.setTextColor(uvIndexColor(g_weather.uvIndex), colorBg);
    screen.drawString(uvLine, leftX, y);
    screen.setTextColor(colorText, colorBg);
  } else {
    screen.drawString("UV Index: --", leftX, y);
  }
  y += 34;

  {
    // Cross-page teaser: countdown to the next sunrise/sunset, so the
    // dashboard hints at the weather page's data without duplicating it.
    if (g_weather.valid && g_weather.sunriseUnix > 0 && g_weather.sunsetUnix > 0) {
      uint32_t nowUnix = (uint32_t)time(nullptr);
      bool isDay = nowUnix >= g_weather.sunriseUnix && nowUnix < g_weather.sunsetUnix;
      uint32_t targetUnix;
      if (isDay) {
        targetUnix = g_weather.sunsetUnix;
      } else if (nowUnix < g_weather.sunriseUnix) {
        targetUnix = g_weather.sunriseUnix;
      } else {
        // Today's sunset has already passed and we haven't polled fresh
        // data yet. Day length shifts by only ~1-2 min/day this time of
        // year, so today's sunrise + 24h is a solid stand-in for
        // tomorrow's sunrise until the next weather poll corrects it.
        targetUnix = g_weather.sunriseUnix + 86400;
      }
      uint32_t secsUntil = (targetUnix > nowUnix) ? (targetUnix - nowUnix) : 0;
      int hh = secsUntil / 3600;
      int mm = (secsUntil % 3600) / 60;
      char teaser[48];
      snprintf(teaser, sizeof(teaser), "%s in %dh %dm", isDay ? "Sunset" : "Sunrise", hh, mm);
      screen.setTextSize(2);
      screen.setTextColor(colorDim, colorBg);
      screen.drawString(teaser, leftX, y);
    }
  }
  y += 40;

  // AIRCRAFT + ISS column -- the second of 2 even columns.
  int y2 = 207;
  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString("OVERHEAD", rightX, y2);
  screen.drawLine(rightX, y2 + 20, rightX + 96, y2 + 20, colorAccent);
  y2 += 30;

  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);
  snprintf(line, sizeof(line), "Aircraft nearby: %d", countVisibleAircraft());
  screen.drawString(line, rightX, y2);
  y2 += 34;

  if (g_iss.valid) {
    snprintf(line, sizeof(line), "ISS altitude: %.0f km", g_iss.altitudeKm);
    screen.drawString(line, rightX, y2);
  } else {
    screen.drawString("ISS: --", rightX, y2);
  }
  y2 += 34;

  {
    // Cross-page teaser: countdown to the next visible ISS pass, so the
    // dashboard hints at the ISS page's data without duplicating it.
    if (g_iss.nextPassUnix > 0) {
      uint32_t nowUnix = (uint32_t)time(nullptr);
      bool visibleNow = nowUnix >= g_iss.nextPassUnix &&
          nowUnix <= g_iss.nextPassUnix + (uint32_t)g_iss.nextPassDurationSec;
      screen.setTextSize(2);
      if (visibleNow) {
        screen.setTextColor(colorSuccess, colorBg);
        screen.drawString("ISS visible now!", rightX, y2);
      } else {
        uint32_t secsUntil = (g_iss.nextPassUnix > nowUnix) ? (g_iss.nextPassUnix - nowUnix) : 0;
        int hh = secsUntil / 3600;
        int mm = (secsUntil % 3600) / 60;
        char teaser[48];
        snprintf(teaser, sizeof(teaser), "Next pass in %dh %dm", hh, mm);
        screen.setTextColor(colorDim, colorBg);
        screen.drawString(teaser, rightX, y2);
      }
    }
  }
  y2 += 44; // a couple spaces below the last OVERHEAD line

  // ASTRO: seeing + transparency + tonight's verdict, stacked below
  // OVERHEAD in the same right-hand column rather than its own column.
  {
    screen.setTextSize(2);
    screen.setTextColor(colorAccent, colorBg);
    screen.drawString("ASTRO", rightX, y2);
    screen.drawLine(rightX, y2 + 20, rightX + 60, y2 + 20, colorAccent);
    y2 += 30;

    int tonightIdx = findTonightAstroIndex();
    screen.setTextSize(2);
    if (tonightIdx >= 0) {
      int seeingVal = g_astroForecast[tonightIdx].seeing;
      char seeingLine[40];
      snprintf(seeingLine, sizeof(seeingLine), "Seeing: %s", astro_seeing_label(seeingVal));
      screen.setTextColor(astroSeverityColor(seeingVal, 8), colorBg);
      screen.drawString(seeingLine, rightX, y2);
      y2 += 34;

      int transVal = g_astroForecast[tonightIdx].transparency;
      char transLine[40];
      snprintf(transLine, sizeof(transLine), "Transparency: %s", astro_transparency_label(transVal));
      screen.setTextColor(astroSeverityColor(transVal, 8), colorBg);
      screen.drawString(transLine, rightX, y2);
      y2 += 34;

      float badness = 0;
      const char* verdict = astro_tonight_verdict(
          g_astroForecast[tonightIdx].cloudcover,
          g_astroForecast[tonightIdx].seeing,
          g_astroForecast[tonightIdx].transparency,
          g_moonIllumPercent, &badness);
      uint16_t verdictColor;
      if (badness < 0.25f) verdictColor = colorSuccess;
      else if (badness < 0.5f) verdictColor = screen.color565(230, 200, 40); // true yellow = FAIR
      else if (badness < 0.75f) verdictColor = screen.color565(230, 130, 40);
      else verdictColor = colorDanger;

      char verdictLine[40];
      snprintf(verdictLine, sizeof(verdictLine), "Tonight: %s", verdict);
      screen.setTextColor(verdictColor, colorBg);
      screen.drawString(verdictLine, rightX, y2);
    } else {
      screen.setTextColor(colorDim, colorBg);
      screen.drawString("Seeing: --", rightX, y2);
      y2 += 34;
      screen.drawString("Transparency: --", rightX, y2);
      y2 += 34;
      screen.drawString("Tonight: --", rightX, y2);
    }
  }
}

static const int RADAR_CX = 320;
static const int RADAR_CY = 260;
static const int RADAR_RADIUS = 190;
static const float RADAR_MAX_RANGE_NM = 40.0f;

static int countVisibleAircraft() {
  int visible = 0;
  for (int i = 0; i < g_aircraftCount; i++) {
    if (g_aircraft[i].distanceNm <= RADAR_MAX_RANGE_NM) visible++;
  }
  return visible;
}
static const int RADAR_RINGS = 4;

static const int MAX_LIST_ROWS = 20;
int g_listRowAircraftIdx[MAX_LIST_ROWS];
int g_listRowY0[MAX_LIST_ROWS];
int g_listRowY1[MAX_LIST_ROWS];
int g_listRowCount = 0;
int g_selectedAircraftIndex = -1;

static void draw_aircraft_detail_card(int listX) {
  Aircraft& a = g_aircraft[g_selectedAircraftIndex];

  int photoBoxW = 300;
  int photoBoxH = 110;
  if (g_aircraftPhotoValid && g_aircraftPhotoPixels != nullptr) {
    int drawW = g_aircraftPhotoWidth < photoBoxW ? g_aircraftPhotoWidth : photoBoxW;
    int drawH = g_aircraftPhotoHeight < photoBoxH ? g_aircraftPhotoHeight : photoBoxH;
    screen.drawRGBBitmap(listX, 55, g_aircraftPhotoPixels, drawW, drawH);
  } else {
    screen.fillRect(listX, 55, photoBoxW, photoBoxH, screen.color565(25, 28, 34));
    screen.setTextSize(1);
    screen.setTextColor(colorDim, screen.color565(25, 28, 34));
    screen.setTextDatum(textdatum_t::middle_center);
    screen.drawString(g_aircraftDetail.lookupInProgress ? "Loading photo..." : "No photo available",
                       listX + photoBoxW / 2, 55 + photoBoxH / 2);
    screen.setTextDatum(textdatum_t::top_left);
  }

  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);
  screen.setTextDatum(textdatum_t::top_left);
  char header[32];
  const char* callsign = a.callsign.length() > 0 ? a.callsign.c_str() : "????";
  snprintf(header, sizeof(header), "%s", callsign);
  screen.drawString(header, listX, 172);

  screen.setTextSize(1);
  screen.setTextColor(colorAccent, colorBg);
  if (g_aircraftDetail.lookupInProgress) {
    screen.drawString("Looking up...", listX, 199);
  } else if (g_aircraftDetail.valid && g_aircraftDetail.type.length() > 0) {
    screen.drawString(g_aircraftDetail.type, listX, 199);
  } else {
    screen.drawString("Type unknown", listX, 199);
  }

  int y = 222;
  char row[48];
  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Altitude", listX, y);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%d ft", a.altitudeFt);
  screen.drawString(row, listX + 100, y);
  y += 20;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("V/S", listX, y);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%+d fpm", a.verticalRateFpm);
  screen.drawString(row, listX + 100, y);
  y += 20;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Speed", listX, y);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%d kt", a.groundSpeedKt);
  screen.drawString(row, listX + 100, y);
  y += 20;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Distance", listX, y);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%.0f nm", a.distanceNm);
  screen.drawString(row, listX + 100, y);
  y += 20;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Bearing", listX, y);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%.0f deg", a.bearingFromHome);
  screen.drawString(row, listX + 100, y);
  y += 20;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Squawk", listX, y);
  screen.setTextColor(colorText, colorBg);
  screen.drawString(a.squawk.length() > 0 ? a.squawk : String("----"), listX + 100, y);
  y += 26;

  screen.setTextColor(colorAccent, colorBg);
  if (g_aircraftDetail.valid && g_aircraftDetail.originIata.length() > 0) {
    char route[64];
    snprintf(route, sizeof(route), "%s -> %s",
             g_aircraftDetail.originIata.c_str(),
             g_aircraftDetail.destIata.length() > 0 ? g_aircraftDetail.destIata.c_str() : "?");
    screen.drawString(route, listX, y);
    y += 20;
    if (g_aircraftDetail.originName.length() > 0) {
      char names[64];
      snprintf(names, sizeof(names), "%s -> %s",
               g_aircraftDetail.originName.c_str(),
               g_aircraftDetail.destName.length() > 0 ? g_aircraftDetail.destName.c_str() : "?");
      screen.drawString(names, listX, y);
    }
  } else if (!g_aircraftDetail.lookupInProgress) {
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("Route unknown", listX, y);
  }

  screen.fillRect(listX, 420, 100, 40, colorAccent);
  screen.setTextColor(colorBg, colorAccent);
  screen.setTextSize(2);
  screen.setTextDatum(textdatum_t::middle_center);
  screen.drawString("< BACK", listX + 50, 440);
  screen.setTextDatum(textdatum_t::top_left);
}

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

  // Classic rotating radar sweep -- a bright leading line plus two dimmer
  // trailing lines behind it, to fake a fading sweep without needing an
  // actual alpha-blended arc (this display library has no fill-alpha).
  // Drawn before the aircraft blips so it sits underneath them.
  {
    uint32_t t = millis();
    static const uint32_t SWEEP_PERIOD_MS = 14000; // slowed from 4000 -- a full spin every 4s
                                                    // read as too fast/frantic for a radar sweep
    float sweepAngle = (float)(t % SWEEP_PERIOD_MS) / (float)SWEEP_PERIOD_MS * 2.0f * PI;
    uint16_t sweepColorMain = colorSuccess;
    // Trail rendered as a single very dim line rather than two -- with only
    // 3 total shades available (no alpha blending on this display), two
    // separate trail lines both landed close to colorDim and read as "two
    // extra white lines" instead of a fading tail.
    uint16_t sweepColorTrail = screen.color565(20, 30, 25);
    float trailOffset = 0.16f;
    {
      float ang = sweepAngle - trailOffset;
      int ex = RADAR_CX + (int)(sinf(ang) * RADAR_RADIUS);
      int ey = RADAR_CY - (int)(cosf(ang) * RADAR_RADIUS);
      screen.drawLine(RADAR_CX, RADAR_CY, ex, ey, sweepColorTrail);
    }
    int sx = RADAR_CX + (int)(sinf(sweepAngle) * RADAR_RADIUS);
    int sy = RADAR_CY - (int)(cosf(sweepAngle) * RADAR_RADIUS);
    screen.drawLine(RADAR_CX, RADAR_CY, sx, sy, sweepColorMain);
  }

  int closestIdx = -1;
  float closestDist = 1e9f;
  bool anyEmergency = false;
  for (int i = 0; i < g_aircraftCount; i++) {
    if (g_aircraft[i].distanceNm > RADAR_MAX_RANGE_NM) continue;
    if (g_aircraft[i].distanceNm < closestDist) {
      closestDist = g_aircraft[i].distanceNm;
      closestIdx = i;
    }
    if (isEmergencySquawk(g_aircraft[i].squawk)) anyEmergency = true;
  }

  for (int i = 0; i < g_aircraftCount; i++) {
    Aircraft& a = g_aircraft[i];
    float rangeFrac = a.distanceNm / RADAR_MAX_RANGE_NM;
    if (rangeFrac > 1.0f) continue;

    float bearingRad = a.bearingFromHome * (PI / 180.0f);
    int px = RADAR_CX + (int)(sinf(bearingRad) * rangeFrac * RADAR_RADIUS);
    int py = RADAR_CY - (int)(cosf(bearingRad) * rangeFrac * RADAR_RADIUS);

    bool isEmergency = isEmergencySquawk(a.squawk);
    uint16_t planeColor = isEmergency ? colorDanger : colorForAltitude(a.altitudeFt);
    int dotRadius = dotRadiusForAltitude(a.altitudeFt);
    screen.fillCircle(px, py, dotRadius, planeColor);
    // Altitude tick removed -- redundant with dot size/color, which
    // already encodes altitude band (see colorForAltitude/dotRadiusForAltitude).

    // Heading vector: a short line showing which way the aircraft is
    // actually pointed (trackDeg), independent of the altitude tick above.
    // Same compass-to-screen convention as the bearing-from-home plotting.
    {
      float headingRad = a.trackDeg * (PI / 180.0f);
      int headingLen = 13;
      int hx = px + (int)(sinf(headingRad) * headingLen);
      int hy = py - (int)(cosf(headingRad) * headingLen);
      screen.drawLine(px, py, hx, hy, planeColor);
    }

    if (i == closestIdx) {
      screen.drawCircle(px, py, dotRadius + 4, colorAccent);
    }
    if (isEmergency) {
      screen.drawCircle(px, py, dotRadius + 6, colorDanger);
    }

    screen.setTextColor(colorLabel, colorBg);
    screen.setTextDatum(textdatum_t::top_left);
    const char* label = a.callsign.length() > 0 ? a.callsign.c_str() : "????";
    screen.drawString(label, px + 8, py - 6);
  }

  {
    float rad45 = 45.0f * PI / 180.0f;
    int rangeLabelX = RADAR_CX + (int)(sinf(rad45) * RADAR_RADIUS);
    int rangeLabelY = RADAR_CY - (int)(cosf(rad45) * RADAR_RADIUS);
    screen.setTextSize(1);
    screen.setTextColor(colorLabel, colorBg);
    screen.setTextDatum(textdatum_t::middle_center);
    char rangeLabel[16];
    snprintf(rangeLabel, sizeof(rangeLabel), "%.0fnm", RADAR_MAX_RANGE_NM);
    screen.drawString(rangeLabel, rangeLabelX, rangeLabelY);

    // Same treatment for the 2nd ring from the middle (ring 2 of 4), so
    // the mid-range distance is labeled too, not just the outer edge.
    int ring2Radius = RADAR_RADIUS * 2 / RADAR_RINGS;
    int ring2LabelX = RADAR_CX + (int)(sinf(rad45) * ring2Radius);
    int ring2LabelY = RADAR_CY - (int)(cosf(rad45) * ring2Radius);
    float ring2Nm = RADAR_MAX_RANGE_NM * 2.0f / RADAR_RINGS;
    char ring2Label[16];
    snprintf(ring2Label, sizeof(ring2Label), "%.0fnm", ring2Nm);
    screen.drawString(ring2Label, ring2LabelX, ring2LabelY);
  }

  {
    // Altitude-color legend, in its own titled column to the left of the
    // radar (radar was shifted right to make room, mirroring the way the
    // aircraft list gets its own column to the right).
    int legX = 20;
    int legY = 130;
    screen.setTextSize(2);
    screen.setTextColor(colorAccent, colorBg);
    screen.setTextDatum(textdatum_t::top_left);
    screen.drawString("ALTITUDE", legX, legY);
    screen.drawLine(legX, legY + 20, legX + 96, legY + 20, colorAccent);
    legY += 44;

    struct LegendEntry { uint16_t color; const char* label; };
    LegendEntry legend[] = {
      { colorForAltitude(2000),  "<5k ft" },
      { colorForAltitude(10000), "5-15k ft" },
      { colorForAltitude(20000), "15-30k ft" },
      { colorForAltitude(35000), ">30k ft" },
    };
    screen.setTextSize(1);
    screen.setTextDatum(textdatum_t::top_left);
    for (int i = 0; i < 4; i++) {
      screen.fillRect(legX, legY - 6, 14, 14, legend[i].color);
      screen.setTextColor(colorLabel, colorBg);
      screen.drawString(legend[i].label, legX + 20, legY - 3);
      legY += 34;
    }

    // Ring indicators are drawn as outlines here (rather than filled
    // swatches) since that's how they actually appear around aircraft
    // dots on the radar.
    legY += 4;
    screen.drawCircle(legX + 7, legY, 7, colorAccent);
    screen.setTextColor(colorLabel, colorBg);
    screen.drawString("Closest aircraft", legX + 20, legY - 3);
    legY += 26;

    screen.drawCircle(legX + 7, legY, 7, colorDanger);
    screen.setTextColor(colorLabel, colorBg);
    screen.drawString("Emergency squawk", legX + 20, legY - 3);

    screen.setTextDatum(textdatum_t::top_left);
  }

  int listX = 530;

  if (g_selectedAircraftIndex >= 0 && g_selectedAircraftIndex < g_aircraftCount) {
    g_listRowCount = 0; // no list rows while the card is showing
    draw_aircraft_detail_card(listX);
    return;
  }

  int visibleCount = countVisibleAircraft();

  int listY = 55;
  if (anyEmergency) {
    screen.setTextSize(1);
    screen.setTextColor(colorDanger, colorBg);
    screen.setTextDatum(textdatum_t::top_left);
    screen.drawString("EMERGENCY SQUAWK DETECTED", listX, listY);
    listY += 20;
  }
  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);
  screen.setTextDatum(textdatum_t::top_left);
  char header[32];
  snprintf(header, sizeof(header), "NEARBY (%d)", visibleCount);
  screen.drawString(header, listX, listY);
  listY += 36;

  screen.setTextSize(1);
  int shown = 0;
  g_listRowCount = 0;

  int sortedIdx[MAX_TRACKED_AIRCRAFT];
  int sortedCount = 0;
  for (int i = 0; i < g_aircraftCount; i++) {
    if (g_aircraft[i].distanceNm <= RADAR_MAX_RANGE_NM) {
      sortedIdx[sortedCount++] = i;
    }
  }
  for (int a = 1; a < sortedCount; a++) {
    int key = sortedIdx[a];
    float keyDist = g_aircraft[key].distanceNm;
    int b = a - 1;
    while (b >= 0 && g_aircraft[sortedIdx[b]].distanceNm > keyDist) {
      sortedIdx[b + 1] = sortedIdx[b];
      b--;
    }
    sortedIdx[b + 1] = key;
  }

  int rowCap = min(MAX_LIST_ROWS, (HEIGHT - listY - 10) / 22);
  for (int s = 0; s < sortedCount && shown < rowCap; s++) {
    int i = sortedIdx[s];
    Aircraft& a = g_aircraft[i];
    bool isEmergency = isEmergencySquawk(a.squawk);
    char row[64];
    const char* callsign = a.callsign.length() > 0 ? a.callsign.c_str() : "????";
    if (isEmergency) {
      snprintf(row, sizeof(row), "%-8s %5dft  %.0fnm  SQ%s", callsign, a.altitudeFt, a.distanceNm, a.squawk.c_str());
    } else {
      snprintf(row, sizeof(row), "%-8s %5dft  %.0fnm", callsign, a.altitudeFt, a.distanceNm);
    }
    screen.fillCircle(listX - 8, listY + 8, 3, isEmergency ? colorDanger : colorForAltitude(a.altitudeFt));
    uint16_t rowColor = colorText;
    if (s == 0) rowColor = colorAccent;
    if (isEmergency) rowColor = colorDanger;
    screen.setTextColor(rowColor, colorBg);
    screen.drawString(row, listX, listY);

    g_listRowAircraftIdx[g_listRowCount] = i;
    g_listRowY0[g_listRowCount] = listY - 2;
    g_listRowY1[g_listRowCount] = listY + 20;
    g_listRowCount++;

    listY += 22;
    shown++;
  }
  if (visibleCount > shown) {
    screen.setTextColor(colorDim, colorBg);
    char more[24];
    snprintf(more, sizeof(more), "+%d more", visibleCount - shown);
    screen.drawString(more, listX, listY);
    listY += 20;
  }
  if (visibleCount == 0) {
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("No aircraft in range", listX, listY);
    listY += 30;
    screen.setTextSize(1);
    char errLine[48];
    snprintf(errLine, sizeof(errLine), "Last HTTP result: %d", g_aviationStatus.lastHttpCode);
    screen.drawString(errLine, listX, listY);
    listY += 20;
    if (g_aviationStatus.lastError.length() > 0) {
      screen.drawString(g_aviationStatus.lastError.c_str(), listX, listY);
    }
  }
}

// ---- Weather icon drawing (plain vector shapes, no bitmap assets) ----

static void drawCloudIcon(int cx, int cy, int r, uint16_t color) {
  screen.fillCircle(cx - (int)(r * 0.6), cy, (int)(r * 0.7), color);
  screen.fillCircle(cx + (int)(r * 0.5), cy, (int)(r * 0.8), color);
  screen.fillCircle(cx, cy - (int)(r * 0.4), (int)(r * 0.9), color);
  screen.fillRect(cx - r, cy, r * 2, (int)(r * 0.7), color);
}

static void drawSunIcon(int cx, int cy, int r, uint16_t color) {
  screen.fillCircle(cx, cy, r, color);
  for (int i = 0; i < 8; i++) {
    float ang = i * (PI / 4.0f);
    int x1 = cx + (int)(cos(ang) * (r + 4));
    int y1 = cy + (int)(sin(ang) * (r + 4));
    int x2 = cx + (int)(cos(ang) * (r + 12));
    int y2 = cy + (int)(sin(ang) * (r + 12));
    screen.drawLine(x1, y1, x2, y2, color);
  }
}

static void drawRainIcon(int cx, int cy, int r, uint16_t color) {
  drawCloudIcon(cx, cy - (int)(r * 0.3), (int)(r * 0.8), color);
  uint16_t rainColor = screen.color565(90, 160, 230);
  for (int i = -1; i <= 1; i++) {
    int x = cx + i * (int)(r * 0.5);
    screen.drawLine(x, cy + (int)(r * 0.5), x - 4, cy + (int)(r * 1.1), rainColor);
  }
}

static void drawSnowIcon(int cx, int cy, int r, uint16_t color) {
  drawCloudIcon(cx, cy - (int)(r * 0.3), (int)(r * 0.8), color);
  uint16_t snowColor = screen.color565(220, 230, 245);
  for (int i = -1; i <= 1; i++) {
    int x = cx + i * (int)(r * 0.5);
    int y = cy + (int)(r * 0.8);
    screen.drawLine(x - 5, y, x + 5, y, snowColor);
    screen.drawLine(x, y - 5, x, y + 5, snowColor);
    screen.drawLine(x - 4, y - 4, x + 4, y + 4, snowColor);
    screen.drawLine(x - 4, y + 4, x + 4, y - 4, snowColor);
  }
}

static void drawStormIcon(int cx, int cy, int r, uint16_t color) {
  drawCloudIcon(cx, cy - (int)(r * 0.3), (int)(r * 0.8), color);
  uint16_t boltColor = screen.color565(240, 210, 60);
  screen.fillTriangle(cx - 2, cy + (int)(r * 0.4), cx + 8, cy + (int)(r * 0.4), cx - 4, cy + (int)(r * 0.9), boltColor);
  screen.fillTriangle(cx - 4, cy + (int)(r * 0.9), cx + 6, cy + (int)(r * 0.9), cx, cy + (int)(r * 1.3), boltColor);
}

static void drawFogIcon(int cx, int cy, int r, uint16_t color) {
  for (int i = -1; i <= 1; i++) {
    screen.drawLine(cx - r, cy + i * (int)(r * 0.4), cx + r, cy + i * (int)(r * 0.4), color);
  }
}

static void drawWeatherIcon(int cx, int cy, int r, int weatherId, uint16_t color) {
  if (weatherId >= 200 && weatherId < 300) {
    drawStormIcon(cx, cy, r, color);
  } else if (weatherId >= 300 && weatherId < 600) {
    drawRainIcon(cx, cy, r, color);
  } else if (weatherId >= 600 && weatherId < 700) {
    drawSnowIcon(cx, cy, r, color);
  } else if (weatherId >= 700 && weatherId < 800) {
    drawFogIcon(cx, cy, r, color);
  } else if (weatherId == 800) {
    uint16_t sunColor = screen.color565(250, 200, 60);
    drawSunIcon(cx, cy, r, sunColor);
  } else if (weatherId > 800) {
    drawCloudIcon(cx, cy, r, color);
  } else {
    screen.drawCircle(cx, cy, r, color);
  }
}

static void formatHHMM(uint32_t unixTime, char* out, size_t outLen) {
  if (unixTime == 0) { snprintf(out, outLen, "--:--"); return; }
  time_t t = (time_t)unixTime;
  struct tm* timeInfo = localtime(&t);
  strftime(out, outLen, "%I:%M %p", timeInfo);
}

// A fun animated background scene that reacts to current conditions -
// drawn first so all the real weather info renders on top of it.
// Everything is time-driven off millis(), so it animates for free just
// by being redrawn every frame - no state needs to be stored.
static void drawWeatherBackground(int weatherId, bool isNight) {
  uint32_t t = millis();

  if (weatherId >= 200 && weatherId < 300) {
    // Thunderstorm: rain + an occasional flash
    if ((t % 4000) < 120) {
      screen.fillRect(0, 0, WIDTH, HEIGHT, screen.color565(60, 60, 70));
    }
  }

  if (weatherId >= 200 && weatherId < 600) {
    // Rain (also covers the drizzle/rain range and storms above)
    uint16_t rainColor = screen.color565(50, 80, 110);
    for (int i = 0; i < 18; i++) {
      int baseX = (i * 47) % WIDTH;
      int y = (int)((t / 6 + (uint32_t)i * 61) % (uint32_t)(HEIGHT + 40)) - 20;
      screen.drawLine(baseX, y, baseX - 6, y + 16, rainColor);
    }
  } else if (weatherId >= 600 && weatherId < 700) {
    // Snow: gently swaying flakes
    uint16_t snowColor = screen.color565(60, 65, 75);
    for (int i = 0; i < 14; i++) {
      int baseX = (i * 59) % WIDTH;
      int y = (int)((t / 12 + (uint32_t)i * 83) % (uint32_t)(HEIGHT + 20)) - 10;
      int sway = (int)(sinf((float)t / 700.0f + (float)i) * 8.0f);
      screen.fillCircle(baseX + sway, y, 2, snowColor);
    }
  }

  if (weatherId == 800) {
    if (isNight) {
      // Clear night: moon + twinkling stars
      uint16_t moonColor = screen.color565(45, 50, 65);
      screen.fillCircle(WIDTH - 90, 80, 34, moonColor);
      screen.fillCircle(WIDTH - 78, 70, 30, colorBg);
      for (int i = 0; i < 20; i++) {
        int sx = (i * 131) % WIDTH;
        int sy = (i * 71) % 220;
        bool twinkle = ((t / 400 + (uint32_t)i) % 5) != 0;
        if (twinkle) {
          screen.drawPixel(sx, sy, screen.color565(50, 55, 65));
        }
      }
    } else {
      // Clear day: gently pulsing sun with rays, tucked in the corner
      uint16_t sunColor = screen.color565(50, 45, 25);
      int cx = WIDTH - 90, cy = 80;
      float pulse = 10.0f + sinf((float)t / 900.0f) * 3.0f;
      for (int i = 0; i < 8; i++) {
        float ang = i * (PI / 4.0f) + (float)t / 4000.0f;
        int x1 = cx + (int)(cosf(ang) * (34.0f + pulse));
        int y1 = cy + (int)(sinf(ang) * (34.0f + pulse));
        int x2 = cx + (int)(cosf(ang) * (48.0f + pulse));
        int y2 = cy + (int)(sinf(ang) * (48.0f + pulse));
        screen.drawLine(x1, y1, x2, y2, sunColor);
      }
      screen.fillCircle(cx, cy, 30, sunColor);
    }
  } else if (weatherId > 800) {
    // Cloudy: several large soft clouds slowly drifting by at different
    // speeds and depths, for a fuller sky.
    uint16_t cloudColor = screen.color565(28, 32, 40);
    static const int cloudCount = 6;
    for (int i = 0; i < cloudCount; i++) {
      int driftSpan = WIDTH + 220;
      uint32_t speed = 30 + (uint32_t)(i % 3) * 15; // vary speed by "depth"
      int x = (int)((t / speed + (uint32_t)i * 170) % (uint32_t)driftSpan) - 110;
      int y = 55 + (i % 4) * 45;
      int size = 30 + (i % 3) * 10;
      drawCloudIcon(x, y, size, cloudColor);
    }
  }
}

static void draw_weather() {
  screen.setTextDatum(textdatum_t::top_left);

  if (!g_weather.valid) {
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("No weather data yet", 20, 100);
    char errLine[48];
    snprintf(errLine, sizeof(errLine), "Last HTTP result: %d", g_weather.lastHttpCode);
    screen.drawString(errLine, 20, 134);
    return;
  }

  bool isNight = false;
  time_t now = time(nullptr);
  if (now > 100000 && g_weather.sunriseUnix > 0 && g_weather.sunsetUnix > 0) {
    isNight = (uint32_t)now < g_weather.sunriseUnix || (uint32_t)now > g_weather.sunsetUnix;
  }
  drawWeatherBackground(g_weather.weatherId, isNight);

  drawWeatherIcon(80, 100, 40, g_weather.weatherId, colorText);

  drawTempF(g_weather.tempF, 150, 70, 4, colorText, colorBg);

  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString(g_weather.condition.c_str(), 150, 130);

  int y = 170;
  screen.setTextSize(2);
  char row[48];

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Feels like", 20, y);
  drawTempF(g_weather.feelsLikeF, 260, y, 2, colorText, colorBg);
  y += 30;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Wind", 20, y);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%.0f mph", g_weather.windMph);
  screen.drawString(row, 260, y);
  y += 30;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Humidity", 20, y);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%d", g_weather.humidity);
  screen.drawString(row, 260, y);
  {
    // Hand-drawn percent glyph: two dots + a diagonal stroke, since this
    // font's charset doesn't render '%' correctly (shows as a placeholder).
    int gx = 260 + 40;
    int gy = y;
    screen.fillCircle(gx, gy + 3, 2, colorText);
    screen.fillCircle(gx + 10, gy + 11, 2, colorText);
    screen.drawLine(gx - 1, gy + 13, gx + 11, gy + 1, colorText);
  }
  y += 30;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Dew Point", 20, y);
  drawTempF(g_weather.dewPointF, 260, y, 2, colorText, colorBg);
  y += 30;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Sunrise", 20, y);
  screen.setTextColor(colorText, colorBg);
  {
    char sunriseBuf[16];
    formatHHMM(g_weather.sunriseUnix, sunriseBuf, sizeof(sunriseBuf));
    screen.drawString(sunriseBuf, 260, y);
  }
  y += 30;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Sunset", 20, y);
  screen.setTextColor(colorText, colorBg);
  {
    char sunsetBuf[16];
    formatHHMM(g_weather.sunsetUnix, sunsetBuf, sizeof(sunsetBuf));
    screen.drawString(sunsetBuf, 260, y);
  }

  {
    // Precipitation gauge: a 270-degree arc (gap at the bottom), approximated
    // with short line segments since this display library doesn't expose a
    // drawArc primitive. A blue segment fills in up to the current percent.
    int gaugeCx = 434, gaugeCy = 122, gaugeR = 18; // moved up another ~1/4in (24px) again,
                                                     // per follow-up request.
    float startDeg = -135.0f, sweepDeg = 270.0f;
    uint16_t trackColor = colorDim;
    uint16_t fillColor = screen.color565(70, 150, 220);

    float prevX = 0, prevY = 0;
    bool havePrev = false;
    for (int i = 0; i <= 90; i++) {
      float deg = startDeg + sweepDeg * (i / 90.0f);
      float rad = deg * PI / 180.0f;
      float px = gaugeCx + sinf(rad) * gaugeR;
      float py = gaugeCy - cosf(rad) * gaugeR;
      if (havePrev) screen.drawLine((int)prevX, (int)prevY, (int)px, (int)py, trackColor);
      prevX = px; prevY = py; havePrev = true;
    }

    float valueFrac = constrain(g_weather.precipChance / 100.0f, 0.0f, 1.0f);
    int valueSteps = (int)(90 * valueFrac);
    havePrev = false;
    for (int i = 0; i <= valueSteps; i++) {
      float deg = startDeg + sweepDeg * (i / 90.0f);
      float rad = deg * PI / 180.0f;
      float px = gaugeCx + sinf(rad) * gaugeR;
      float py = gaugeCy - cosf(rad) * gaugeR;
      if (havePrev) screen.drawLine((int)prevX, (int)prevY, (int)px, (int)py, fillColor);
      prevX = px; prevY = py; havePrev = true;
    }

    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.setTextDatum(textdatum_t::top_left);
    screen.drawString("PRECIP", gaugeCx - 36, gaugeCy + gaugeR + 18);

    screen.setTextSize(2);
    screen.setTextColor(colorText, colorBg);
    char precipStr[8];
    snprintf(precipStr, sizeof(precipStr), "%d", g_weather.precipChance);
    screen.drawString(precipStr, gaugeCx - 12, gaugeCy + gaugeR + 48);
    {
      // Hand-drawn percent glyph, same trick used for humidity above.
      int gx = gaugeCx + 8;
      int gy = gaugeCy + gaugeR + 54;
      screen.fillCircle(gx, gy - 5, 2, colorText);
      screen.fillCircle(gx + 8, gy + 3, 2, colorText);
      screen.drawLine(gx - 1, gy + 5, gx + 9, gy - 7, colorText);
    }
    screen.setTextDatum(textdatum_t::top_left);
  }

  {
    // Wind compass: direction needle plus sustained | gust speeds below.
    int windCx = 434, windCy = 252, windR = 18; // moved up in lockstep with the precip gauge
                                                 // above (same 24px shift), keeping the vertical
                                                 // gap between the two unchanged.
    screen.drawCircle(windCx, windCy, windR, colorDim);

    for (int deg = 0; deg < 360; deg += 30) {
      float rad = deg * PI / 180.0f;
      bool isCardinal = (deg % 90 == 0);
      int tickLen = isCardinal ? 8 : 4;
      int x0 = windCx + (int)(sinf(rad) * windR);
      int y0 = windCy - (int)(cosf(rad) * windR);
      int x1 = windCx + (int)(sinf(rad) * (windR - tickLen));
      int y1 = windCy - (int)(cosf(rad) * (windR - tickLen));
      screen.drawLine(x0, y0, x1, y1, isCardinal ? colorText : colorDim);
    }

    screen.drawLine(windCx - 6, windCy, windCx + 6, windCy, colorDim);
    screen.drawLine(windCx, windCy - 6, windCx, windCy + 6, colorDim);

    float windRad = g_weather.windDeg * PI / 180.0f;
    int tipX = windCx + (int)(sinf(windRad) * (windR - 10));
    int tipY = windCy - (int)(cosf(windRad) * (windR - 10));
    uint16_t needleColor = colorText;
    screen.drawLine(windCx, windCy, tipX, tipY, needleColor);
    float leftRad = windRad + 2.7f;
    float rightRad = windRad - 2.7f;
    int lx = tipX - (int)(sinf(leftRad) * 8);
    int ly = tipY + (int)(cosf(leftRad) * 8);
    int rx = tipX - (int)(sinf(rightRad) * 8);
    int ry = tipY + (int)(cosf(rightRad) * 8);
    screen.fillTriangle(tipX, tipY, lx, ly, rx, ry, needleColor);

    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.setTextDatum(textdatum_t::top_left);
    screen.drawString("WINDS", windCx - 30, windCy + windR + 12);

    screen.setTextSize(2);
    screen.setTextColor(colorText, colorBg);
    char windStr[24];
    snprintf(windStr, sizeof(windStr), "%.0f / %.0f", g_weather.windMph, g_weather.windGustMph);
    // Center dynamically based on actual string length, since digit count
    // varies (e.g. "1 / 3" vs "12 / 18") -- this font is monospace-ish at
    // roughly 12px/char at text size 2.
    int windStrWidth = (int)strlen(windStr) * 12;
    screen.drawString(windStr, windCx - windStrWidth / 2, windCy + windR + 38);
    screen.setTextDatum(textdatum_t::top_left);
  }

  {
    int aqX = 520;
    int aqY = 60;
    screen.setTextSize(2);
    screen.setTextColor(colorAccent, colorBg);
    screen.setTextDatum(textdatum_t::top_left);
    screen.drawString("AIR QUALITY", aqX, aqY);
    screen.drawLine(aqX, aqY + 20, aqX + 132, aqY + 20, colorAccent);
    aqY += 48;

    if (g_airQuality.valid) {
      uint16_t aqiColor = airQualityColor(g_airQuality.aqi);
      screen.setTextSize(4);
      screen.setTextColor(aqiColor, colorBg);
      char aqiNum[8];
      snprintf(aqiNum, sizeof(aqiNum), "%d", g_airQuality.aqi);
      screen.drawString(aqiNum, aqX, aqY);

      screen.setTextSize(2);
      screen.setTextColor(colorText, colorBg);
      screen.drawString(air_quality_label(g_airQuality.aqi), aqX + 60, aqY + 16);
      aqY += 60;

      // Continuous gradient bar with a pointer marking exactly where
      // today's AQI falls, rather than 5 flat lit/unlit segments -- the
      // number now means something at a glance against the full scale.
      int barW = 200, barH = 14;
      for (int px = 0; px < barW; px += 2) {
        float frac = (float)px / (float)(barW - 1);
        screen.fillRect(aqX + px, aqY, 2, barH, aqiGradientColor(frac));
      }
      float aqiFrac = constrain((float)(g_airQuality.aqi - 1) / 4.0f, 0.0f, 1.0f);
      int aqiPointerX = aqX + (int)(aqiFrac * (barW - 1));
      screen.fillTriangle(aqiPointerX - 5, aqY - 6, aqiPointerX + 5, aqY - 6, aqiPointerX, aqY - 1, colorText);
      aqY += barH + 24;
    } else {
      screen.setTextSize(2);
      screen.setTextColor(colorDim, colorBg);
      screen.drawString("--", aqX, aqY);
      aqY += 30;
      char errLine[32];
      snprintf(errLine, sizeof(errLine), "HTTP %d", g_airQuality.lastHttpCode);
      screen.drawString(errLine, aqX, aqY);
      aqY += 30;
    }

    // UV INDEX -- same block layout as Air Quality above, sourced from
    // Open-Meteo (see fetchUvIndex() in weather_service.cpp), since
    // OpenWeatherMap's free tier doesn't include UV data.
    screen.setTextSize(2);
    screen.setTextColor(colorAccent, colorBg);
    screen.drawString("UV INDEX", aqX, aqY);
    screen.drawLine(aqX, aqY + 20, aqX + 96, aqY + 20, colorAccent);
    aqY += 48;

    if (g_weather.uvValid) {
      uint16_t uvColor = uvIndexColor(g_weather.uvIndex);
      screen.setTextSize(4);
      screen.setTextColor(uvColor, colorBg);
      char uvNum[8];
      snprintf(uvNum, sizeof(uvNum), "%.0f", g_weather.uvIndex);
      screen.drawString(uvNum, aqX, aqY);

      screen.setTextSize(2);
      screen.setTextColor(colorText, colorBg);
      screen.drawString(uvIndexLabel(g_weather.uvIndex), aqX + 60, aqY + 16);
      aqY += 60;

      int uvBarW = 200, uvBarH = 14;
      for (int px = 0; px < uvBarW; px += 2) {
        float frac = (float)px / (float)(uvBarW - 1);
        screen.fillRect(aqX + px, aqY, 2, uvBarH, uvGradientColor(frac));
      }
      float uvFrac = constrain(g_weather.uvIndex / 11.0f, 0.0f, 1.0f);
      int uvPointerX = aqX + (int)(uvFrac * (uvBarW - 1));
      screen.fillTriangle(uvPointerX - 5, aqY - 6, uvPointerX + 5, aqY - 6, uvPointerX, aqY - 1, colorText);
    } else {
      screen.setTextSize(2);
      screen.setTextColor(colorDim, colorBg);
      screen.drawString("--", aqX, aqY);
      aqY += 30;
      char errLine[32];
      snprintf(errLine, sizeof(errLine), "HTTP %d", g_weather.uvLastHttpCode);
      screen.drawString(errLine, aqX, aqY);
    }
    screen.setTextSize(2);
    screen.setTextColor(colorText, colorBg);
  }

  int stripY = 360;
  screen.drawLine(20, stripY - 11, WIDTH - 20, stripY - 11, colorDim);

  int colW = (WIDTH - 40) / 5;
  screen.setTextDatum(textdatum_t::middle_center);
  for (int i = 0; i < g_forecastCount; i++) {
    int cx = 20 + colW * i + colW / 2;

    screen.setTextSize(2);
    screen.setTextColor(colorText, colorBg);
    screen.drawString(g_forecast[i].dayLabel, cx, stripY + 4);

    drawWeatherIcon(cx, stripY + 40, 20, g_forecast[i].weatherId, colorText);

    char hilo[24];
    snprintf(hilo, sizeof(hilo), "%.0f / %.0f", g_forecast[i].highF, g_forecast[i].lowF);
    screen.drawString(hilo, cx, stripY + 74);
  }
  screen.setTextDatum(textdatum_t::top_left);
}

static String formatUnixTime(uint32_t unixTime) {
  if (unixTime == 0) return "unknown";
  time_t t = (time_t)unixTime;
  struct tm* timeInfo = localtime(&t);
  char buf[32];
  strftime(buf, sizeof(buf), "%a %I:%M %p", timeInfo);
  return String(buf);
}

// Compact time like "9:43P" for the visible-passes list, where space is tight.
static void formatPassTime(uint32_t unixTime, char* out, size_t outLen) {
  if (unixTime == 0) { snprintf(out, outLen, "--"); return; }
  time_t t = (time_t)unixTime;
  struct tm* ti = localtime(&t);
  int h12 = ti->tm_hour % 12;
  if (h12 == 0) h12 = 12;
  snprintf(out, outLen, "%d:%02d%s", h12, ti->tm_min, ti->tm_hour < 12 ? "A" : "P");
}

// Compact date like "Jul18" for the visible-passes list.
static void formatPassDate(uint32_t unixTime, char* out, size_t outLen) {
  if (unixTime == 0) { snprintf(out, outLen, "--"); return; }
  time_t t = (time_t)unixTime;
  struct tm* ti = localtime(&t);
  strftime(out, outLen, "%b%d", ti);
}

static void drawIssIcon(int cx, int cy, uint16_t color) {
  screen.fillRect(cx - 3, cy - 3, 6, 6, color);
  screen.fillRect(cx - 15, cy - 2, 9, 4, color);
  screen.fillRect(cx + 6, cy - 2, 9, 4, color);

  // A pulsing halo ring around the real (live) position -- makes clear
  // this is the "right now" marker versus the static ground-track line
  // drawn behind it, without inventing any predicted future path.
  float pulsePhase = (float)(millis() % 2000) / 2000.0f; // 0..1 over 2s
  int haloR = 9 + (int)(pulsePhase * 6.0f);               // grows from 9 to 15px
  screen.drawCircle(cx, cy, haloR, color);
}

static void draw_iss() {
  screen.setTextDatum(textdatum_t::top_left);

  if (!g_iss.valid) {
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("No ISS data yet", 20, 100);
    screen.setTextSize(2);
    char errLine[48];
    snprintf(errLine, sizeof(errLine), "Last HTTP result: %d", g_iss.lastHttpCode);
    screen.drawString(errLine, 20, 140);
    screen.drawString("(200=ok, 401/403=bad key, neg=connection error)", 20, 175);
    return;
  }

  const int MAP_X = 20, MAP_Y = 55, MAP_W = 760, MAP_H = 200;
  uint16_t colorGrid = screen.color565(40, 60, 80);
  uint16_t colorEquator = screen.color565(70, 100, 130);
  uint16_t colorIss = screen.color565(255, 90, 90);

  screen.drawLine(MAP_X, MAP_Y, MAP_X + MAP_W, MAP_Y, colorGrid);
  screen.drawLine(MAP_X, MAP_Y + MAP_H, MAP_X + MAP_W, MAP_Y + MAP_H, colorGrid);
  screen.drawLine(MAP_X, MAP_Y, MAP_X, MAP_Y + MAP_H, colorGrid);
  screen.drawLine(MAP_X + MAP_W, MAP_Y, MAP_X + MAP_W, MAP_Y + MAP_H, colorGrid);
  for (int lon = -150; lon <= 150; lon += 30) {
    int x = MAP_X + (int)((lon + 180) / 360.0f * MAP_W);
    screen.drawLine(x, MAP_Y, x, MAP_Y + MAP_H, colorGrid);
  }
  for (int lat = -60; lat <= 60; lat += 30) {
    int y = MAP_Y + (int)((90 - lat) / 180.0f * MAP_H);
    screen.drawLine(MAP_X, y, MAP_X + MAP_W, y, colorGrid);
  }
  int equatorY = MAP_Y + MAP_H / 2;
  int primeMeridianX = MAP_X + MAP_W / 2;
  screen.drawLine(MAP_X, equatorY, MAP_X + MAP_W, equatorY, colorEquator);
  screen.drawLine(primeMeridianX, MAP_Y, primeMeridianX, MAP_Y + MAP_H, colorEquator);

  int homeX = MAP_X + (int)((HOME_LON + 180) / 360.0f * MAP_W);
  int homeY = MAP_Y + (int)((90 - HOME_LAT) / 180.0f * MAP_H);
  uint16_t colorHome = colorSuccess;
  screen.fillTriangle(homeX - 7, homeY, homeX + 7, homeY, homeX, homeY - 9, colorHome);
  screen.fillRect(homeX - 5, homeY, 10, 7, colorHome);
  screen.setTextSize(1);
  screen.setTextColor(colorHome, colorBg);
  screen.setTextDatum(textdatum_t::top_left);
  screen.drawString("Home", homeX + 10, homeY - 4);

  if (g_issTrackValid && g_issTrackCount > 1) {
    uint16_t colorTrack = screen.color565(255, 170, 60);
    int prevX = 0, prevY = 0;
    bool havePrev = false;
    for (int i = 0; i < g_issTrackCount; i++) {
      int px = MAP_X + (int)((g_issTrack[i].lon + 180) / 360.0f * MAP_W);
      int py = MAP_Y + (int)((90 - g_issTrack[i].lat) / 180.0f * MAP_H);
      if (havePrev && abs(px - prevX) < MAP_W / 2) {
        screen.drawLine(prevX, prevY, px, py, colorTrack);
      }
      prevX = px;
      prevY = py;
      havePrev = true;
    }
  } else {
    // No ground track without a successfully loaded TLE -- show why on
    // screen, since serial logging hasn't been reliably captured tonight.
    char tleLine[64];
    snprintf(tleLine, sizeof(tleLine), "TLE HTTP: %d", g_tleLastHttpCode);
    screen.setTextSize(1);
    screen.setTextColor(colorDim, colorBg);
    screen.setTextDatum(textdatum_t::top_left);
    screen.drawString(tleLine, MAP_X + 4, MAP_Y + 4);
    if (g_tleLastFailureReason.length() > 0) {
      screen.drawString(g_tleLastFailureReason, MAP_X + 4, MAP_Y + 18);
    }
  }

  int issX = MAP_X + (int)((g_iss.lon + 180) / 360.0f * MAP_W);
  int issY = MAP_Y + (int)((90 - g_iss.lat) / 180.0f * MAP_H);
  drawIssIcon(issX, issY, colorIss);

  screen.setTextSize(1);
  screen.setTextColor(colorIss, colorBg);
  char posLabel[32];
  snprintf(posLabel, sizeof(posLabel), "%.2f, %.2f", g_iss.lat, g_iss.lon);
  int posLabelX = issX + 16;
  if (posLabelX + 80 > MAP_X + MAP_W) posLabelX = issX - 96; // flip left near the edge
  screen.drawString(posLabel, posLabelX, issY - 6);

  int belowY = MAP_Y + MAP_H + 15;
  int col1X = 20;
  int col2X = 300;
  int col3X = 550;

  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.setTextDatum(textdatum_t::top_left);
  screen.drawString("POSITION", col1X, belowY);
  screen.drawLine(col1X, belowY + 20, col1X + 96, belowY + 20, colorAccent);
  screen.drawString("NEXT PASS", col2X, belowY);
  screen.drawLine(col2X, belowY + 20, col2X + 108, belowY + 20, colorAccent);
  screen.drawString("PASSES", col3X, belowY);
  screen.drawLine(col3X, belowY + 20, col3X + 72, belowY + 20, colorAccent);

  int contentY = belowY + 30;
  char row[64];

  // Column 1: Current Position
  screen.setTextSize(2);
  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Latitude", col1X, contentY);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%.2f", g_iss.lat);
  screen.drawString(row, col1X + 110, contentY);

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Longitude", col1X, contentY + 32);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%.2f", g_iss.lon);
  screen.drawString(row, col1X + 130, contentY + 32);

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Altitude", col1X, contentY + 64);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%.0f km", g_iss.altitudeKm);
  screen.drawString(row, col1X + 110, contentY + 64);

  // Column 2: Next Visible Pass -- countdown boxes or a "visible now" banner
  uint32_t nowUnix = (uint32_t)time(nullptr);
  bool isVisibleNow = g_iss.nextPassUnix > 0 &&
      nowUnix >= g_iss.nextPassUnix &&
      nowUnix <= g_iss.nextPassUnix + (uint32_t)g_iss.nextPassDurationSec;

  if (isVisibleNow) {
    screen.setTextSize(2);
    screen.setTextColor(colorSuccess, colorBg);
    screen.drawString("VISIBLE NOW!", col2X, contentY);
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("Duration", col2X, contentY + 32);
    screen.setTextColor(colorText, colorBg);
    snprintf(row, sizeof(row), "%d min", g_iss.nextPassDurationSec / 60);
    screen.drawString(row, col2X + 100, contentY + 32);
  } else if (g_iss.nextPassUnix > 0) {
    uint32_t secsUntil = (g_iss.nextPassUnix > nowUnix) ? (g_iss.nextPassUnix - nowUnix) : 0;
    int hh = secsUntil / 3600;
    int mm = (secsUntil % 3600) / 60;
    int ss = secsUntil % 60;
    if (hh > 99) hh = 99;

    int boxY = contentY;
    int boxW = 46, boxH = 46, gap = 8, colonW = 16;
    uint16_t boxColor = screen.color565(30, 34, 45);
    char digitBuf[3];
    int cx = col2X;

    screen.setTextDatum(textdatum_t::middle_center);

    screen.fillRect(cx, boxY, boxW, boxH, boxColor);
    snprintf(digitBuf, sizeof(digitBuf), "%02d", hh);
    screen.setTextSize(3);
    screen.setTextColor(colorText, boxColor);
    screen.drawString(digitBuf, cx + boxW / 2, boxY + boxH / 2);
    cx += boxW + gap;

    screen.setTextColor(colorDim, colorBg);
    screen.drawString(":", cx + colonW / 2, boxY + boxH / 2);
    cx += colonW + gap;

    screen.fillRect(cx, boxY, boxW, boxH, boxColor);
    snprintf(digitBuf, sizeof(digitBuf), "%02d", mm);
    screen.setTextColor(colorText, boxColor);
    screen.drawString(digitBuf, cx + boxW / 2, boxY + boxH / 2);
    cx += boxW + gap;

    screen.setTextColor(colorDim, colorBg);
    screen.drawString(":", cx + colonW / 2, boxY + boxH / 2);
    cx += colonW + gap;

    screen.fillRect(cx, boxY, boxW, boxH, boxColor);
    snprintf(digitBuf, sizeof(digitBuf), "%02d", ss);
    screen.setTextColor(colorText, boxColor);
    screen.drawString(digitBuf, cx + boxW / 2, boxY + boxH / 2);

    screen.setTextDatum(textdatum_t::top_left);

    int detailY = boxY + boxH + 10;
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("Duration", col2X, detailY);
    screen.setTextColor(colorText, colorBg);
    snprintf(row, sizeof(row), "%d min", g_iss.nextPassDurationSec / 60);
    screen.drawString(row, col2X + 100, detailY);

    if (g_issPassCount > 0) {
      // Look-direction compass, matching the wind compass style on the
      // Weather page: a small dial with cardinal ticks and a needle
      // pointing where to look at the pass's peak.
      int compassCx = col2X + 22, compassCy = detailY + 46, compassR = 22;
      screen.drawCircle(compassCx, compassCy, compassR, colorDim);
      for (int deg = 0; deg < 360; deg += 30) {
        float rad = deg * PI / 180.0f;
        bool isCardinal = (deg % 90 == 0);
        int tickLen = isCardinal ? 6 : 3;
        int x0 = compassCx + (int)(sinf(rad) * compassR);
        int y0 = compassCy - (int)(cosf(rad) * compassR);
        int x1 = compassCx + (int)(sinf(rad) * (compassR - tickLen));
        int y1 = compassCy - (int)(cosf(rad) * (compassR - tickLen));
        screen.drawLine(x0, y0, x1, y1, isCardinal ? colorText : colorDim);
      }
      float azRad = g_issPasses[0].maxAz * PI / 180.0f;
      int tipX = compassCx + (int)(sinf(azRad) * (compassR - 6));
      int tipY = compassCy - (int)(cosf(azRad) * (compassR - 6));
      screen.drawLine(compassCx, compassCy, tipX, tipY, colorSuccess);
      screen.fillCircle(tipX, tipY, 3, colorSuccess);

      char maxElLine[24];
      snprintf(maxElLine, sizeof(maxElLine), "Max El %d", g_issPasses[0].maxElevationDeg);
      screen.setTextColor(colorText, colorBg);
      screen.drawString(maxElLine, compassCx + compassR + 12, compassCy - 10);
    }

    if (g_issCrewCount == 0) {
      snprintf(row, sizeof(row), "Crew Aboard 0 (HTTP %d)", g_issCrewLastHttpCode);
    } else {
      snprintf(row, sizeof(row), "Crew Aboard %d", g_issCrewCount);
    }
    screen.drawString(row, col2X, detailY + 84);
  } else {
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("No upcoming pass found", col2X, contentY);
  }

  // Column 3: Visible Passes list -- up to 3 rows, columns aligned via
  // fixed-width padding (this font is monospace, so %-Ns keeps every
  // row's DATE/START/EL lined up under one another). End time and
  // magnitude were dropped from this table to fit the larger text --
  // date/start/elevation are the more useful at-a-glance fields.
  {
    screen.setTextSize(2);
    int rowY = contentY;

    screen.setTextColor(colorDim, colorBg);
    screen.drawString("DATE  START  EL", col3X, rowY);
    rowY += 24;

    // Passes-fetch diagnostic line (HTTP code / parse status) that lived
    // here during the maxEl=0 bug hunt has been hidden now that it's
    // fixed and confirmed working -- the underlying tracking
    // (g_issPassesLastHttpCode / g_issPassesParseFailed) is left in place
    // in case it's needed again.

    int shownPasses = min(g_issPassCount, 3);
    if (shownPasses == 0) {
      screen.setTextColor(colorDim, colorBg);
      screen.drawString("No passes in the", col3X, rowY);
      screen.drawString("next few days", col3X, rowY + 24);
    } else {
      for (int i = 0; i < shownPasses; i++) {
        IssPass& p = g_issPasses[i];
        char line[64];
        char passDateBuf[16];
        char passTimeBuf[16];
        formatPassDate(p.startUnix, passDateBuf, sizeof(passDateBuf));
        formatPassTime(p.startUnix, passTimeBuf, sizeof(passTimeBuf));
        snprintf(line, sizeof(line), "%-6s%-7sEl%-3d",
                 passDateBuf,
                 passTimeBuf,
                 p.maxElevationDeg);
        screen.setTextColor(colorText, colorBg);
        screen.drawString(line, col3X, rowY);
        rowY += 24;
      }
    }
  }
}

// A simple, original decorative badge for the Debug page -- a generic
// heraldic shield silhouette (rounded top, tapering sides to a point;
// a common, non-branded shape used widely, not matching any specific
// company's trademark) containing a compass rose (echoing the Aviation
// page) and an orbiting satellite dot with a center star (echoing the
// ISS/Astro pages). All diagnostic readouts were previously here; they
// are intentionally hidden per request, not removed from the codebase
// logic (the NEXT/poll buttons still function via their existing touch
// coordinates in screen_manager_handle_touch, just without a visible
// button drawn here).
// Draws line segments 3 times, offset by 1px along the perpendicular of
// the segment's direction, to simulate a thicker stroke -- this display
// library has no line-width parameter.
static void drawThickLine(int x0, int y0, int x1, int y1, uint16_t color) {
  screen.drawLine(x0, y0, x1, y1, color);
  float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 0.001f) return;
  int px = (int)roundf(-dy / len);
  int py = (int)roundf(dx / len);
  screen.drawLine(x0 + px, y0 + py, x1 + px, y1 + py, color);
  screen.drawLine(x0 - px, y0 - py, x1 - px, y1 - py, color);
}

static void drawThickCircle(int cx, int cy, int r, uint16_t color) {
  screen.drawCircle(cx, cy, r, color);
  screen.drawCircle(cx, cy, r - 1, color);
  screen.drawCircle(cx, cy, r + 1, color);
}

static void drawDebugBadge() {
  int cx = WIDTH / 2;
  static const int HEADER_H = 40;
  int shieldR = 130;                    // slightly smaller than before to fit below the header
  int shieldTopCy = HEADER_H + 10 + shieldR;  // top of the arc sits just below the header band
  int apexY = HEIGHT - 20;

  uint16_t shieldColor = screen.color565(190, 60, 60);     // deep red outline
  uint16_t compassColor = screen.color565(190, 195, 205);  // silver
  uint16_t compassAccent = screen.color565(230, 230, 240); // bright silver for cardinal ticks
  uint16_t starColor = screen.color565(235, 195, 70);      // gold
  uint16_t issColor = screen.color565(150, 170, 235);      // pale blue
  uint16_t planeColor = screen.color565(120, 200, 150);    // soft green
  uint16_t galaxyColor = screen.color565(170, 120, 210);   // violet
  uint16_t galaxyCoreColor = screen.color565(230, 210, 255);

  // Real system-health metrics feeding this animation's timing below --
  // same visual as before, but now doubling as a living system monitor
  // rather than pure decoration. No RTT tracking exists anywhere in this
  // project, so WiFi signal strength (already used for the Dashboard's
  // signal-bar icon) stands in as the network-health proxy instead.
  size_t freeHeap = esp_get_free_heap_size();
  size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  float fragRatio = (freeHeap > 0) ? (float)largestBlock / (float)freeHeap : 1.0f; // 1.0 = unfragmented
  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -100;

  // Galaxy spin: healthier (less fragmented) heap spins faster.
  uint32_t galaxyPeriodMs = (uint32_t)constrain(14000.0f - fragRatio * 10000.0f, 4000.0f, 14000.0f);
  // ISS orbit: more free heap overall spins faster.
  float heapFrac = constrain((float)((int)freeHeap - 50000) / (float)(250000 - 50000), 0.0f, 1.0f);
  uint32_t orbitPeriodMs = (uint32_t)(12000.0f - heapFrac * 9000.0f);
  // Plane speed: stronger WiFi signal flies faster (lower divisor = faster).
  float rssiFrac = constrain((float)(rssi + 90) / 60.0f, 0.0f, 1.0f); // -90(0) .. -30(1)
  uint32_t planeDivisor = (uint32_t)(34.0f - rssiFrac * 20.0f); // 34 (weak) .. 14 (strong)

  // Shield outline, thicker stroke.
  float prevX = 0, prevY = 0;
  bool havePrev = false;
  for (int i = 0; i <= 60; i++) {
    float deg = 180.0f + 180.0f * (i / 60.0f);
    float rad = deg * PI / 180.0f;
    float px = cx + cosf(rad) * shieldR;
    float py = shieldTopCy + sinf(rad) * shieldR;
    if (havePrev) drawThickLine((int)prevX, (int)prevY, (int)px, (int)py, shieldColor);
    prevX = px; prevY = py; havePrev = true;
  }
  int leftShoulderX = cx - shieldR, rightShoulderX = cx + shieldR;
  drawThickLine(leftShoulderX, shieldTopCy, cx, apexY, shieldColor);
  drawThickLine(rightShoulderX, shieldTopCy, cx, apexY, shieldColor);

  // Compass rose, thicker ring.
  int compassCy = shieldTopCy + 55;
  int compassR = 96;
  drawThickCircle(cx, compassCy, compassR, compassColor);
  for (int deg = 0; deg < 360; deg += 30) {
    float rad = deg * PI / 180.0f;
    bool isCardinal = (deg % 90 == 0);
    int tickLen = isCardinal ? 10 : 5;
    int x0 = cx + (int)(sinf(rad) * compassR);
    int y0 = compassCy - (int)(cosf(rad) * compassR);
    int x1 = cx + (int)(sinf(rad) * (compassR - tickLen));
    int y1 = compassCy - (int)(cosf(rad) * (compassR - tickLen));
    drawThickLine(x0, y0, x1, y1, isCardinal ? compassAccent : compassColor);
  }

  // Small rotating galaxy -- a spiral of dots around a bright core -- tucked
  // in the upper part of the shield, above the compass rose.
  {
    int galaxyCx = cx, galaxyCy = shieldTopCy - shieldR / 2 + 8;
    uint32_t t = millis();
    float baseAngle = (float)(t % galaxyPeriodMs) / (float)galaxyPeriodMs * 2.0f * PI;
    for (int i = 0; i < 10; i++) {
      float armAngle = baseAngle + (float)i * (2.0f * PI / 10.0f);
      float armR = 6.0f + (float)i * 1.8f;
      int dx = galaxyCx + (int)(cosf(armAngle) * armR);
      int dy = galaxyCy + (int)(sinf(armAngle) * armR * 0.6f); // flattened, spiral-galaxy look
      screen.fillCircle(dx, dy, 2, galaxyColor);
    }
    screen.fillCircle(galaxyCx, galaxyCy, 4, galaxyCoreColor);
  }

  // Orbit ring with an ISS-style icon (body + solar panel arms, not just a
  // dot) drifting around it.
  int orbitR = 58;
  drawThickCircle(cx, compassCy, orbitR, compassColor);
  uint32_t t = millis();
  float orbitAngle = (float)(t % orbitPeriodMs) / (float)orbitPeriodMs * 2.0f * PI;
  int satX = cx + (int)(cosf(orbitAngle) * orbitR);
  int satY = compassCy + (int)(sinf(orbitAngle) * orbitR);
  screen.fillRect(satX - 3, satY - 3, 6, 6, issColor);
  screen.fillRect(satX - 12, satY - 2, 8, 4, issColor);
  screen.fillRect(satX + 4, satY - 2, 8, 4, issColor);

  // A little airplane silhouette drifting across the FULL screen width
  // (not just the shield), echoing the Dashboard's own background
  // animation. Each pass picks a fresh random vertical position, so the
  // flight path varies across any portion of the screen instead of
  // repeating the same line every lap.
  {
    static const int HEADER_H = 40;
    static int planeY = -1;
    static int lastLap = -1;
    int span = WIDTH + 80;
    uint32_t cycle = t / planeDivisor;
    int x = (int)(cycle % (uint32_t)span) - 40;
    int lap = (int)(cycle / (uint32_t)span);
    if (lap != lastLap || planeY < 0) {
      lastLap = lap;
      planeY = HEADER_H + 40 + (int)(esp_random() % (uint32_t)(HEIGHT - HEADER_H - 80));
    }
    int px = x, py = planeY;
    screen.fillRect(px - 14, py - 2, 20, 4, planeColor);
    screen.fillTriangle(px + 4, py - 3, px + 4, py + 3, px + 13, py, planeColor);
    screen.fillTriangle(px + 1, py, px - 7, py - 11, px - 2, py, planeColor);
    screen.fillTriangle(px + 1, py, px - 7, py + 11, px - 2, py, planeColor);
    screen.fillTriangle(px - 11, py, px - 16, py - 6, px - 13, py, planeColor);
    screen.fillTriangle(px - 11, py, px - 16, py + 6, px - 13, py, planeColor);
  }

  // Center star, gold.
  int starOuterR = 27, starInnerR = 12;
  float starPtsX[10], starPtsY[10];
  for (int i = 0; i < 10; i++) {
    float ang = -PI / 2.0f + i * (PI / 5.0f);
    float r = (i % 2 == 0) ? (float)starOuterR : (float)starInnerR;
    starPtsX[i] = cx + cosf(ang) * r;
    starPtsY[i] = compassCy + sinf(ang) * r;
  }
  for (int i = 0; i < 10; i++) {
    int next = (i + 1) % 10;
    screen.fillTriangle(cx, compassCy, (int)starPtsX[i], (int)starPtsY[i],
                        (int)starPtsX[next], (int)starPtsY[next], starColor);
  }
}

// Small legend explaining what each animated element's speed actually
// tracks now that the badge doubles as a real system-health monitor
// (see the metrics computed at the top of drawDebugBadge()). Placed in
// the open left margin, which stays clear of the shield shape at every
// height on this page.
static void drawDebugLegend() {
  uint16_t planeColor = screen.color565(120, 200, 150);
  uint16_t issColor = screen.color565(150, 170, 235);
  uint16_t galaxyColor = screen.color565(170, 120, 210);

  int legX = 20;
  int legY = 55;
  int swatchSize = 12;
  int lineGap = 26;

  struct LegendEntry { uint16_t color; const char* label; };
  LegendEntry legend[] = {
    { planeColor,  "PLANE = WIFI SIGNAL" },
    { issColor,    "ORBIT = FREE HEAP" },
    { galaxyColor, "GALAXY = HEAP HEALTH" },
  };

  screen.setTextSize(1);
  screen.setTextDatum(textdatum_t::top_left);
  for (int i = 0; i < 3; i++) {
    screen.fillRect(legX, legY - 2, swatchSize, swatchSize, legend[i].color);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString(legend[i].label, legX + swatchSize + 8, legY);
    legY += lineGap;
  }
}

static void draw_debug() {
  drawDebugBadge();
  drawDebugLegend();
}

// Finds the first astro forecast point at or after tonight's sunset --
// a reasonable stand-in for "tonight's conditions" without needing exact
// astronomical twilight calculations.
static int findTonightAstroIndex() {
  if (g_astroForecastCount == 0) return -1;
  if (g_weather.sunsetUnix == 0) return 0;
  for (int i = 0; i < g_astroForecastCount; i++) {
    if (g_astroForecast[i].unixTime >= g_weather.sunsetUnix) return i;
  }
  return g_astroForecastCount - 1;
}

// Colors a 1-8 or 1-9 severity index green-to-red, reused across seeing,
// transparency, and cloud cover regardless of each scale's exact size.
static uint16_t astroSeverityColor(int idx, int maxIdx) {
  float frac = (float)(idx - 1) / (float)(maxIdx - 1);
  if (frac < 0.25f) return colorSuccess;
  if (frac < 0.5f)  return screen.color565(230, 200, 40); // true yellow = FAIR
  if (frac < 0.75f) return screen.color565(230, 130, 40);
  return colorDanger;
}

// Finds the best (lowest composite badness) point across all the astro
// forecast data we already have (~48 hours), restricted to nighttime
// hours (20:00-05:59 local) since seeing/transparency only matter after
// dark. Reuses today's moon illumination as an approximation across the
// whole window -- moon phase barely shifts over 2 days.
static int findBestNightIndex(float* outBadness) {
  int bestIdx = -1;
  float bestBadness = 2.0f; // worse than any real value (max is 1.0)
  for (int i = 0; i < g_astroForecastCount; i++) {
    time_t t = (time_t)g_astroForecast[i].unixTime;
    struct tm* ti = localtime(&t);
    bool isNight = (ti->tm_hour >= 20 || ti->tm_hour < 6);
    if (!isNight) continue;

    float badness = 0;
    astro_tonight_verdict(g_astroForecast[i].cloudcover, g_astroForecast[i].seeing,
                           g_astroForecast[i].transparency, g_moonIllumPercent, &badness);
    if (badness < bestBadness) {
      bestBadness = badness;
      bestIdx = i;
    }
  }
  if (outBadness) *outBadness = bestBadness;
  return bestIdx;
}

static void draw_astro() {
  screen.setTextDatum(textdatum_t::top_left);

  astro_recompute_moon_phase();
  int tonightIdx = findTonightAstroIndex();

  // Twinkling starfield, drawn first so all the real text/panels render on
  // top of it. Density and twinkle speed scale with tonight's combined
  // seeing+transparency score (both use the same 1-8 scale, 8 = best) --
  // sharper actual conditions means more visible stars twinkling faster,
  // a small but honest echo of what the sky is actually doing tonight.
  {
    int qualitySum = 8; // neutral default when no data yet
    if (tonightIdx >= 0) {
      qualitySum = g_astroForecast[tonightIdx].seeing + g_astroForecast[tonightIdx].transparency;
    }
    int starCount = constrain(qualitySum * 2, 10, 32);       // more stars when conditions are better
    uint32_t twinkleDivisor = constrain(500 - qualitySum * 20, 150, 500); // smaller = faster twinkle
    uint32_t t = millis();
    uint16_t starColor = screen.color565(70, 75, 90);
    for (int i = 0; i < starCount; i++) {
      int sx = (i * 137) % WIDTH;
      int sy = 45 + (i * 53) % 260;
      bool twinkle = ((t / twinkleDivisor + (uint32_t)i) % 5) != 0;
      if (twinkle) {
        screen.drawPixel(sx, sy, starColor);
      }
    }
  }

  if (tonightIdx < 0) {
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("No astro data yet", 460, 390);
    char httpLine[48];
    snprintf(httpLine, sizeof(httpLine), "Last HTTP result: %d", g_astroLastHttpCode);
    screen.drawString(httpLine, 460, 418);
    screen.drawString("(-999=never tried, neg=connection error)", 460, 446);
    screen.setTextSize(1);
    screen.drawString(g_astroLastFailureReason, 460, 470);
  }

  {
    // Tonight's Verdict -- a single composite score combining cloud cover,
    // seeing, transparency, and moon brightness, so you don't have to
    // mentally combine four separate readouts every time.
    screen.setTextSize(2);
    screen.setTextColor(colorAccent, colorBg);
    screen.drawString("TONIGHT'S VERDICT", 20, 55);
    screen.drawLine(20, 75, 20 + 18 * 12, 75, colorAccent);

    // Static reference value for this location -- Bortle class doesn't
    // change day to day like the rest of this page, so it's a hardcoded
    // constant rather than a network fetch (see HOME_BORTLE_CLASS).
    char bortleLine[24];
    snprintf(bortleLine, sizeof(bortleLine), "Bortle %.1f (Home)", (double)HOME_BORTLE_CLASS);
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString(bortleLine, 560, 55);

    {
      float bestBadness = 0;
      int bestIdx = findBestNightIndex(&bestBadness);
      if (bestIdx >= 0) {
        uint16_t bestColor;
        if (bestBadness < 0.25f) bestColor = colorSuccess;
        else if (bestBadness < 0.5f) bestColor = screen.color565(230, 200, 40); // true yellow = FAIR
        else if (bestBadness < 0.75f) bestColor = screen.color565(230, 130, 40);
        else bestColor = colorDanger;

        char bestLine[32];
        char bestDateBuf[16];
        char bestTimeBuf[16];
        formatPassDate(g_astroForecast[bestIdx].unixTime, bestDateBuf, sizeof(bestDateBuf));
        formatPassTime(g_astroForecast[bestIdx].unixTime, bestTimeBuf, sizeof(bestTimeBuf));
        snprintf(bestLine, sizeof(bestLine), "Best: %s %s",
                 bestDateBuf,
                 bestTimeBuf);
        screen.setTextColor(bestColor, colorBg);
        screen.drawString(bestLine, 560, 79);
      }
    }

    if (tonightIdx >= 0) {
      float badness = 0;
      const char* verdict = astro_tonight_verdict(
          g_astroForecast[tonightIdx].cloudcover,
          g_astroForecast[tonightIdx].seeing,
          g_astroForecast[tonightIdx].transparency,
          g_moonIllumPercent, &badness);
      uint16_t verdictColor;
      if (badness < 0.25f) verdictColor = colorSuccess;
      else if (badness < 0.5f) verdictColor = screen.color565(230, 200, 40); // true yellow = FAIR
      else if (badness < 0.75f) verdictColor = screen.color565(230, 130, 40);
      else verdictColor = colorDanger;

      screen.setTextSize(3);
      screen.setTextColor(verdictColor, colorBg);
      screen.drawString(verdict, 20, 89);
    } else {
      screen.setTextSize(2);
      screen.setTextColor(colorDim, colorBg);
      screen.drawString("--", 20, 89);
    }
  }

  int panelY = 125;
  int col1X = 20, col2X = 290, col3X = 560;

  struct AstroPanel {
    int x;
    const char* title;
    int value;
    int maxValue;
    const char* label;
  };

  int seeingVal = tonightIdx >= 0 ? g_astroForecast[tonightIdx].seeing : 0;
  int transVal  = tonightIdx >= 0 ? g_astroForecast[tonightIdx].transparency : 0;
  int cloudVal  = tonightIdx >= 0 ? g_astroForecast[tonightIdx].cloudcover : 0;

  AstroPanel panels[3] = {
    { col1X, "SEEING",       seeingVal, 8, astro_seeing_label(seeingVal) },
    { col2X, "TRANSPARENCY", transVal,  8, astro_transparency_label(transVal) },
    { col3X, "CLOUD COVER",  cloudVal,  9, astro_cloudcover_label(cloudVal) },
  };

  for (int i = 0; i < 3; i++) {
    AstroPanel& p = panels[i];
    screen.setTextSize(2);
    screen.setTextColor(colorAccent, colorBg);
    screen.drawString(p.title, p.x, panelY);
    int titleWidth = (int)strlen(p.title) * 12;
    screen.drawLine(p.x, panelY + 20, p.x + titleWidth, panelY + 20, colorAccent);

    if (tonightIdx >= 0) {
      screen.setTextSize(3);
      screen.setTextColor(astroSeverityColor(p.value, p.maxValue), colorBg);
      screen.drawString(p.label, p.x, panelY + 34);

      char idxLine[16];
      snprintf(idxLine, sizeof(idxLine), "(%d/%d)", p.value, p.maxValue);
      screen.setTextSize(2);
      screen.setTextColor(colorDim, colorBg);
      screen.drawString(idxLine, p.x, panelY + 70);
    } else {
      screen.setTextSize(2);
      screen.setTextColor(colorDim, colorBg);
      screen.drawString("--", p.x, panelY + 34);
    }
  }

  int row2Y = 225;

  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString("MOON", col1X, row2Y);
  screen.drawLine(col1X, row2Y + 20, col1X + 48, row2Y + 20, colorAccent);

  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);
  screen.drawString(g_moonPhaseLabel, col1X, row2Y + 34);
  char moonPct[24];
  snprintf(moonPct, sizeof(moonPct), "%.0f pct illuminated", g_moonIllumPercent);
  screen.setTextSize(2);
  screen.setTextColor(colorDim, colorBg);
  screen.drawString(moonPct, col1X, row2Y + 62);

  {
    int moonCx = col1X + 353, moonCy = row2Y + 40, moonR = 32;
    screen.fillCircle(moonCx, moonCy, moonR, screen.color565(230, 230, 210));
    float shadowFrac = g_moonPhaseFraction;
    bool waxing = shadowFrac < 0.5f;
    float distFrac = fabsf(shadowFrac - 0.5f) * 2.0f;
    int shadowOffset = (int)(moonR * 2 * distFrac) - moonR;
    int shadowCx = waxing ? moonCx - moonR - shadowOffset : moonCx + moonR + shadowOffset;
    screen.fillCircle(shadowCx, moonCy, moonR, colorBg);
  }

  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString("STORM RISK", col3X, row2Y);
  screen.drawLine(col3X, row2Y + 20, col3X + 132, row2Y + 20, colorAccent);
  if (tonightIdx >= 0) {
    int li = g_astroForecast[tonightIdx].liftedindex;
    screen.setTextSize(2);
    screen.setTextColor(li > 0 ? colorSuccess : colorDanger, colorBg);
    screen.drawString(astro_instability_label(li), col3X, row2Y + 34);

    // Tiny legend under the value: all 4 possible Storm Risk levels,
    // best to worst, each in its matching color -- same GOOD/FAIR/POOR/BAD
    // color scale used elsewhere on this page (astroSeverityColor).
    {
      screen.setTextSize(1);
      int legendY = row2Y + 56;
      int legendX = col3X;
      const char* labels[4] = {"Stable", "Slight", "Moderate", "or High Risk"};
      uint16_t colors[4] = {
        colorSuccess,
        screen.color565(230, 200, 40),
        screen.color565(230, 130, 40),
        colorDanger
      };
      char legendLine[8];
      screen.setTextColor(colorDim, colorBg);
      screen.drawString("(", legendX, legendY);
      legendX += 6;
      for (int i = 0; i < 4; i++) {
        screen.setTextColor(colors[i], colorBg);
        screen.drawString(labels[i], legendX, legendY);
        legendX += strlen(labels[i]) * 6;
        if (i < 3) {
          screen.setTextColor(colorDim, colorBg);
          screen.drawString(",", legendX, legendY);
          legendX += 6;
        }
      }
      screen.setTextColor(colorDim, colorBg);
      screen.drawString(")", legendX, legendY);
      (void)legendLine; // unused, kept for potential future formatting
    }
    if (g_astroForecast[tonightIdx].prectype != "none") {
      char precipLine[32];
      snprintf(precipLine, sizeof(precipLine), "Precip: %s", g_astroForecast[tonightIdx].prectype.c_str());
      screen.setTextSize(2);
      screen.setTextColor(colorDim, colorBg);
      screen.drawString(precipLine, col3X, row2Y + 62);
    }
  } else {
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("--", col3X, row2Y + 34);
  }

  int lineY = 315;
  int stripY = 350;
  screen.drawLine(20, lineY, WIDTH - 20, lineY, colorDim);

  screen.setTextSize(2);
  screen.setTextColor(colorDim, colorBg);
  screen.drawString("SEE", 20, stripY + 6);
  screen.drawString("TRN", 20, stripY + 28);
  screen.drawString("CLD", 20, stripY + 50);

  int stripStartX = 70;
  int colW = (WIDTH - 40 - stripStartX) / 6;
  int startIdx = tonightIdx >= 0 ? tonightIdx : 0;

  // Which forecast index is the single best night-time window overall.
  // The real best night (from findBestNightIndex, searching the full ~48h
  // forecast) is often NOT one of the 6 columns shown here -- e.g. if
  // tonight is poor but tomorrow night is great, the true best index falls
  // outside this strip and would never get highlighted. So: prefer the
  // global best index when it's actually visible in this strip, otherwise
  // fall back to whichever of the 6 visible columns is best, so there's
  // always a highlighted column on screen.
  int bestIdxForStrip = -1;
  {
    float globalBestBadness = 0;
    int globalBestIdx = findBestNightIndex(&globalBestBadness);
    bool globalBestVisible = globalBestIdx >= startIdx && globalBestIdx < startIdx + 6;
    if (globalBestVisible) {
      bestIdxForStrip = globalBestIdx;
    } else {
      float bestVisibleBadness = 2.0f;
      for (int i = 0; i < 6 && (startIdx + i) < g_astroForecastCount; i++) {
        AstroForecastPoint& pt = g_astroForecast[startIdx + i];
        float badness = 0;
        astro_tonight_verdict(pt.cloudcover, pt.seeing, pt.transparency, g_moonIllumPercent, &badness);
        if (badness < bestVisibleBadness) {
          bestVisibleBadness = badness;
          bestIdxForStrip = startIdx + i;
        }
      }
    }
  }

  for (int i = 0; i < 6 && (startIdx + i) < g_astroForecastCount; i++) {
    AstroForecastPoint& pt = g_astroForecast[startIdx + i];
    int cx = stripStartX + colW * i + colW / 2;

    time_t t = (time_t)pt.unixTime;
    struct tm* ti = localtime(&t);
    char timeLabel[8];
    int h12 = ti->tm_hour % 12;
    if (h12 == 0) h12 = 12;
    snprintf(timeLabel, sizeof(timeLabel), "%d%s", h12, ti->tm_hour < 12 ? "A" : "P");
    screen.setTextSize(2);
    screen.setTextDatum(textdatum_t::middle_center);
    screen.setTextColor(colorText, colorBg);
    screen.drawString(timeLabel, cx, stripY - 16);

    screen.fillRect(cx - 20, stripY, 40, 16, astroSeverityColor(pt.seeing, 8));
    screen.fillRect(cx - 20, stripY + 22, 40, 16, astroSeverityColor(pt.transparency, 8));
    screen.fillRect(cx - 20, stripY + 44, 40, 16, astroSeverityColor(pt.cloudcover, 9));

    if (startIdx + i == bestIdxForStrip) {
      // Border drawn with 4 lines (not drawRect) to match every other
      // outline in this file, which builds rectangles the same way.
      // Top edge sits just above the SEE row rather than up at the time
      // label -- the box previously started at stripY-20, which cut
      // straight through the time text sitting at stripY-16.
      int bx0 = cx - 24, bx1 = cx + 24;
      int by0 = stripY - 4, by1 = stripY + 60;
      screen.drawLine(bx0, by0, bx1, by0, colorAccent);
      screen.drawLine(bx0, by1, bx1, by1, colorAccent);
      screen.drawLine(bx0, by0, bx0, by1, colorAccent);
      screen.drawLine(bx1, by0, bx1, by1, colorAccent);
    }
  }

  // Color key: explains the green-to-red severity scale shared by the
  // SEEING/TRANSPARENCY/CLOUD COVER panels and the SEE/TRN/CLD strip below.
  {
    screen.setTextDatum(textdatum_t::top_left);
    int legendY = 428;

    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("KEY", 20, legendY + 2);

    struct LegendItem { uint16_t color; const char* label; };
    LegendItem items[4] = {
      { colorSuccess,                    "GOOD" },
      { screen.color565(230, 200, 40),   "FAIR" },
      { screen.color565(230, 130, 40),   "POOR" },
      { colorDanger,                     "BAD"  },
    };

    int lx = 90;
    for (int i = 0; i < 4; i++) {
      screen.fillRect(lx, legendY, 24, 20, items[i].color);
      screen.setTextColor(colorText, colorBg);
      screen.drawString(items[i].label, lx + 32, legendY + 2);
      lx += 32 + (int)strlen(items[i].label) * 12 + 30;
    }
  }

  screen.setTextDatum(textdatum_t::top_left);
}

// Draws one sparkline panel: a title, a simple min/max-scaled line plot
// across all valid samples, and the current/latest value called out in
// the corner. `getValue` returns the metric for a given sample index, or
// false if that sample has no data for this metric (e.g. before the
// astro forecast first loaded) -- gaps are simply skipped rather than
// plotted as zero, so a temporarily-missing feed doesn't fake a crash to
// zero on the chart.
static void drawTrendPanel(int x, int y, int w, int h, const char* title,
                            bool (*getValue)(int idx, float* outValue), uint16_t lineColor) {
  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.setTextDatum(textdatum_t::top_left);
  screen.drawString(title, x, y);
  int titleWidth = (int)strlen(title) * 12;
  screen.drawLine(x, y + 20, x + titleWidth, y + 20, colorAccent);

  int plotY = y + 30;
  int plotH = h - 30;
  // Built from 4 lines rather than drawRect(), which this display's
  // Canvas class doesn't implement -- same pattern used for every other
  // rectangle outline in this file (e.g. the Astro best-window border).
  screen.drawLine(x, plotY, x + w, plotY, colorDim);
  screen.drawLine(x, plotY + plotH, x + w, plotY + plotH, colorDim);
  screen.drawLine(x, plotY, x, plotY + plotH, colorDim);
  screen.drawLine(x + w, plotY, x + w, plotY + plotH, colorDim);

  if (g_trendSampleCount < 2) {
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.setTextDatum(textdatum_t::middle_center);
    screen.drawString("Collecting data...", x + w / 2, plotY + plotH / 2);
    screen.setTextDatum(textdatum_t::top_left);
    return;
  }

  // Oldest-to-newest sample order in the ring buffer: g_trendSampleCount
  // may be less than TREND_MAX_SAMPLES (still filling up for the first
  // 24h), in which case index 0 is the oldest and g_trendNextWriteIdx is
  // meaningless; once full, the oldest sample is at g_trendNextWriteIdx.
  int oldestIdx = (g_trendSampleCount < TREND_MAX_SAMPLES) ? 0 : g_trendNextWriteIdx;

  float minV = 1e9f, maxV = -1e9f;
  bool anyValid = false;
  for (int i = 0; i < g_trendSampleCount; i++) {
    int idx = (oldestIdx + i) % TREND_MAX_SAMPLES;
    float v;
    if (getValue(idx, &v)) {
      if (v < minV) minV = v;
      if (v > maxV) maxV = v;
      anyValid = true;
    }
  }
  if (!anyValid) {
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.setTextDatum(textdatum_t::middle_center);
    screen.drawString("No data yet", x + w / 2, plotY + plotH / 2);
    screen.setTextDatum(textdatum_t::top_left);
    return;
  }
  if (maxV - minV < 0.001f) { minV -= 1.0f; maxV += 1.0f; } // avoid a flat divide-by-zero line

  int prevPx = 0, prevPy = 0;
  bool havePrev = false;
  float lastValidValue = 0;
  for (int i = 0; i < g_trendSampleCount; i++) {
    int idx = (oldestIdx + i) % TREND_MAX_SAMPLES;
    float v;
    if (!getValue(idx, &v)) {
      havePrev = false; // gap in data -- break the line rather than bridging it
      continue;
    }
    lastValidValue = v;
    int px = x + (int)((float)i / (float)(g_trendSampleCount - 1) * (w - 1));
    float frac = (v - minV) / (maxV - minV);
    int py = plotY + plotH - 1 - (int)(frac * (plotH - 1));
    if (havePrev) {
      screen.drawLine(prevPx, prevPy, px, py, lineColor);
    }
    prevPx = px; prevPy = py; havePrev = true;
  }

  screen.setTextSize(1);
  screen.setTextColor(colorText, colorBg);
  screen.setTextDatum(textdatum_t::top_right);
  char nowLabel[16];
  snprintf(nowLabel, sizeof(nowLabel), "%.0f", lastValidValue);
  screen.drawString(nowLabel, x + w - 4, y + 2);
  screen.setTextDatum(textdatum_t::top_left);
}

static bool trendGetTemp(int idx, float* outValue) {
  if (g_trendSamples[idx].tempF == 0) return false;
  *outValue = g_trendSamples[idx].tempF;
  return true;
}
static bool trendGetAqi(int idx, float* outValue) {
  if (g_trendSamples[idx].aqi == 0) return false;
  *outValue = (float)g_trendSamples[idx].aqi;
  return true;
}
static bool trendGetAircraft(int idx, float* outValue) {
  *outValue = (float)g_trendSamples[idx].aircraftCount;
  return true; // 0 is a legitimate value here (genuinely no aircraft nearby)
}
static bool trendGetAstro(int idx, float* outValue) {
  if (g_trendSamples[idx].astroBadness < 0) return false;
  *outValue = g_trendSamples[idx].astroBadness * 100.0f; // 0..100 reads better than 0..1
  return true;
}

static void draw_trends() {
  screen.setTextDatum(textdatum_t::top_left);

  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);
  char header[48];
  int hoursCovered = (g_trendSampleCount * (int)(TREND_SAMPLE_INTERVAL_MS / 1000)) / 3600;
  // No tilde -- this custom bitmap font's charset doesn't include "~"
  // (renders as a placeholder glyph, looked like a stray question mark).
  snprintf(header, sizeof(header), "Last %d hours (%d samples)", hoursCovered, g_trendSampleCount);
  screen.drawString(header, 20, 50);

  int panelW = (WIDTH - 60) / 2;
  int panelH = 170;
  int col1X = 20, col2X = 20 + panelW + 20;
  int row1Y = 85, row2Y = row1Y + panelH + 20;

  drawTrendPanel(col1X, row1Y, panelW, panelH, "TEMP (F)", trendGetTemp, colorAccent);
  drawTrendPanel(col2X, row1Y, panelW, panelH, "AIR QUALITY (AQI)", trendGetAqi, screen.color565(230, 130, 40));
  drawTrendPanel(col1X, row2Y, panelW, panelH, "AIRCRAFT NEARBY", trendGetAircraft, screen.color565(90, 200, 255));
  drawTrendPanel(col2X, row2Y, panelW, panelH, "ASTRO BADNESS (0=best)", trendGetAstro, screen.color565(170, 120, 210));
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
  colorBgDay = screen.color565(10, 12, 16);
  colorTextDay = screen.color565(235, 240, 245);
  colorDimDay = screen.color565(120, 130, 140);
  colorAccentDay = screen.color565(70, 130, 220);
  colorSuccessDay = screen.color565(80, 200, 120);
  colorDangerDay = screen.color565(220, 80, 80);

  // Night mode: shades of red only, to preserve night vision for
  // astrophotography. Meaning that used to come from hue (blue vs green
  // vs orange) now comes from brightness instead.
  colorBgNight = screen.color565(10, 0, 0);
  colorTextNight = screen.color565(210, 40, 40);
  colorDimNight = screen.color565(90, 15, 15);
  colorAccentNight = screen.color565(160, 30, 30);
  colorSuccessNight = screen.color565(180, 50, 50);
  colorDangerNight = screen.color565(255, 60, 60);

  colorBg = colorBgDay;
  colorText = colorTextDay;
  colorDim = colorDimDay;
  colorAccent = colorAccentDay;
  colorSuccess = colorSuccessDay;
  colorDanger = colorDangerDay;

  lastInteractionMs = millis();
  lastAutoAdvanceMs = millis();
}

// Night (red-shifted) mode is purely manual -- a long-press anywhere
// toggles g_nightModeOn directly in screen_manager_handle_touch(). No
// automatic sunset/sunrise detection.
static bool computeNightModeActive() {
  return g_nightModeOn;
}

static bool g_nightModeActive = false; // updated once per frame below

// Checks the three alert conditions against last frame's state, firing
// the banner exactly once at the moment any of them newly becomes true.
static void checkAlertTriggers() {
  bool astroIsGood = false;
  int tonightIdx = findTonightAstroIndex();
  if (tonightIdx >= 0) {
    float badness = 0;
    astro_tonight_verdict(g_astroForecast[tonightIdx].cloudcover, g_astroForecast[tonightIdx].seeing,
                           g_astroForecast[tonightIdx].transparency, g_moonIllumPercent, &badness);
    astroIsGood = badness < 0.25f; // same GOOD threshold used elsewhere on the Astro/Dashboard pages
  }

  bool stormIsHigh = false;
  if (tonightIdx >= 0) {
    stormIsHigh = g_astroForecast[tonightIdx].liftedindex <= -8; // matches astro_instability_label's "High Risk"
  }

  bool anyEmergencyNow = false;
  for (int i = 0; i < g_aircraftCount; i++) {
    if (isEmergencySquawk(g_aircraft[i].squawk)) { anyEmergencyNow = true; break; }
  }

  if (g_alertStatePrimed) {
    if (anyEmergencyNow && !g_prevAnyEmergency) {
      g_alertActive = true;
      g_alertShownAtMs = millis();
      g_alertColorOverride = colorDanger;
      snprintf(g_alertMessage, sizeof(g_alertMessage), "EMERGENCY SQUAWK DETECTED NEARBY");
    } else if (stormIsHigh && !g_prevStormWasHigh) {
      g_alertActive = true;
      g_alertShownAtMs = millis();
      g_alertColorOverride = colorDanger;
      snprintf(g_alertMessage, sizeof(g_alertMessage), "ASTRO STORM RISK: HIGH RISK TONIGHT");
    } else if (astroIsGood && !g_prevAstroWasGood) {
      g_alertActive = true;
      g_alertShownAtMs = millis();
      g_alertColorOverride = colorSuccess;
      snprintf(g_alertMessage, sizeof(g_alertMessage), "ASTRO CONDITIONS NOW GOOD TONIGHT");
    }
  }

  g_prevAstroWasGood = astroIsGood;
  g_prevStormWasHigh = stormIsHigh;
  g_prevAnyEmergency = anyEmergencyNow;
  g_alertStatePrimed = true;

  if (g_alertActive && millis() - g_alertShownAtMs > ALERT_DURATION_MS) {
    g_alertActive = false;
  }
}

// Drawn last, on top of everything else (including the header), so it's
// a true takeover regardless of which page is showing or whether the
// page is locked.
static void drawAlertBanner() {
  if (!g_alertActive) return;
  int bannerH = 50;
  screen.fillRect(0, 0, WIDTH, bannerH, colorBg);
  screen.drawLine(0, bannerH - 3, WIDTH, bannerH - 3, g_alertColorOverride);
  screen.drawLine(0, bannerH - 2, WIDTH, bannerH - 2, g_alertColorOverride);
  screen.drawLine(0, bannerH - 1, WIDTH, bannerH - 1, g_alertColorOverride);
  screen.setTextSize(2);
  screen.setTextColor(g_alertColorOverride, colorBg);
  screen.setTextDatum(textdatum_t::middle_center);
  screen.drawString(g_alertMessage, WIDTH / 2, bannerH / 2 - 6);
  screen.setTextSize(1);
  screen.setTextColor(colorDim, colorBg);
  screen.drawString("tap to dismiss", WIDTH / 2, bannerH / 2 + 14);
  screen.setTextDatum(textdatum_t::top_left);
}

void screen_manager_draw() {
  g_nightModeActive = computeNightModeActive();
  colorBg = g_nightModeActive ? colorBgNight : colorBgDay;
  colorText = g_nightModeActive ? colorTextNight : colorTextDay;
  colorDim = g_nightModeActive ? colorDimNight : colorDimDay;
  colorAccent = g_nightModeActive ? colorAccentNight : colorAccentDay;
  colorSuccess = g_nightModeActive ? colorSuccessNight : colorSuccessDay;
  colorDanger = g_nightModeActive ? colorDangerNight : colorDangerDay;

  // Idle auto-cycle: once nobody has touched the screen for
  // IDLE_TIMEOUT_MS, advance to the next tab every AUTO_CYCLE_INTERVAL_MS.
  // Any touch (handled in screen_manager_handle_touch) resets the idle
  // clock, so this stops immediately as soon as someone interacts.
  uint32_t nowMs = millis();
  if (!g_pageLocked && nowMs - lastInteractionMs > IDLE_TIMEOUT_MS) {
    if (nowMs - lastAutoAdvanceMs > AUTO_CYCLE_INTERVAL_MS) {
      currentTab = (currentTab + 1) % TAB_COUNT;
      lastAutoAdvanceMs = nowMs;
    }
  }

  screen.fillScreen(colorBg);
  drawHeader();

  switch (currentTab) {
    case 0: draw_dashboard(); break;
    case 1: draw_aviation(); break;
    case 2: draw_astro(); break;
    case 3: draw_iss(); break;
    case 4: draw_weather(); break;
    case 5: draw_debug(); break;
    case 6: draw_trends(); break;
  }

  screen.setTextSize(1);
  screen.setTextColor(colorDim, colorBg);
  screen.setTextDatum(textdatum_t::top_right);
  screen.drawString(FIRMWARE_VERSION, WIDTH - 6, HEIGHT - 14);

  if (g_nightModeActive) {
    screen.setTextColor(colorDim, colorBg);
    screen.setTextDatum(textdatum_t::top_left);
    screen.drawString("NIGHT", 10, HEIGHT - 14);
  }

  if (g_pageLocked) {
    screen.setTextSize(1);
    screen.setTextColor(colorDanger, colorBg);
    screen.setTextDatum(textdatum_t::top_right);
    screen.drawString("LOCKED", WIDTH - 6, HEIGHT - 28);
  }

  checkAlertTriggers();
  drawAlertBanner();
}

static const int DEBUG_TAB_INDEX = 5;
static uint16_t lastTouchX = 0;
static uint16_t lastTouchY = 0;

static const int AVIATION_TAB_INDEX = 1;

void screen_manager_handle_touch(bool touched, uint16_t x, uint16_t y) {
  uint32_t now = millis();
  if (touched) {
    lastInteractionMs = now; // any touch resets the idle auto-cycle clock
    // Remember the position while the finger is actually down - on
    // release, readTouch() reports no point and x/y come in as 0,0,
    // so we can't rely on the release-time coordinates directly.
    lastTouchX = x;
    lastTouchY = y;
    if (!touchWasDown) {
      touchDownMs = now;
      touchDownX = x;
      touchDownY = y;
      touchMinX = x;
      touchMaxX = x;
      touchMinY = y;
      touchMaxY = y;
    } else {
      if (x < touchMinX) touchMinX = x;
      if (x > touchMaxX) touchMaxX = x;
      if (y < touchMinY) touchMinY = y;
      if (y > touchMaxY) touchMaxY = y;
    }
  }
  if (!touched && touchWasDown && g_alertActive) {
    // Any tap while the banner is up just dismisses it -- swallowed here
    // before swipe/longpress/tab-advance logic ever runs, regardless of
    // gesture shape, so dismissing never doubles as page navigation.
    g_alertActive = false;
    touchWasDown = touched;
    return;
  }
  if (!touched && touchWasDown) {
    uint32_t held = now - touchDownMs;
    int leftExcursion = (int)touchDownX - (int)touchMinX;   // how far left of start the finger reached
    int rightExcursion = (int)touchMaxX - (int)touchDownX;  // how far right of start the finger reached
    int downExcursion = (int)touchMaxY - (int)touchDownY;   // how far below start the finger reached
    int upExcursion = (int)touchDownY - (int)touchMinY;     // how far above start the finger reached
    int horizontalPeak = max(leftExcursion, rightExcursion);

    bool isVerticalSwipeDown = downExcursion >= VERTICAL_SWIPE_MIN_PX &&
                               downExcursion > horizontalPeak &&
                               downExcursion > upExcursion;
    bool isHorizontalSwipe = !isVerticalSwipeDown &&
                             horizontalPeak >= SWIPE_MIN_PX &&
                             horizontalPeak > downExcursion &&
                             horizontalPeak > upExcursion;

    if (isVerticalSwipeDown) {
      // Top-to-bottom swipe toggles the page lock -- the current page
      // keeps drawing and updating live data normally, it just stops
      // advancing to the next tab (via swipe, tap-to-advance, or idle
      // auto-cycle) until swiped down again.
      g_pageLocked = !g_pageLocked;
    } else if (isHorizontalSwipe) {
      // Horizontal swipe pages left/right, suppressed while page-locked --
      // the whole point of the lock is to stay put until it's explicitly
      // unlocked. Whichever direction had the larger excursion wins, in
      // case both got a little jitter -- moving left advances forward,
      // matching the usual photo-gallery convention.
      if (!g_pageLocked) {
        if (leftExcursion > rightExcursion) {
          currentTab = (currentTab + 1) % TAB_COUNT;
        } else {
          currentTab = (currentTab - 1 + TAB_COUNT) % TAB_COUNT;
        }
      }
    } else if (held >= LONGPRESS_MIN_MS) {
      // Long press anywhere toggles night mode on/off directly -- no
      // automatic sunset/sunrise trigger. Not page navigation, so this
      // still works even while page-locked.
      g_nightModeOn = !g_nightModeOn;
    } else if (held >= TAP_MIN_MS && held <= TAP_MAX_MS) {
      // Everything below still runs even while page-locked -- aircraft
      // selection and the Debug page's hidden buttons aren't page
      // navigation, so there's no reason to block them. Only the final
      // tap-to-advance fallback at the bottom is suppressed.
      bool hitNextButton = currentTab == DEBUG_TAB_INDEX &&
                            lastTouchX >= 600 && lastTouchX <= 780 &&
                            lastTouchY >= 400 && lastTouchY <= 460;
      bool hitPollButton = currentTab == DEBUG_TAB_INDEX &&
                            lastTouchX >= 230 && lastTouchX <= 410 &&
                            lastTouchY >= 310 && lastTouchY <= 370;
      // Hidden test button for the alert takeover banner -- bottom-left
      // corner of the Debug page, same invisible-touch-zone convention as
      // the two buttons above. Lets the banner/dismiss behavior be
      // verified on demand instead of waiting for a real condition
      // transition (astro-good, storm-risk-high, emergency squawk).
      bool hitAlertTestButton = currentTab == DEBUG_TAB_INDEX &&
                                lastTouchX >= 20 && lastTouchX <= 200 &&
                                lastTouchY >= 400 && lastTouchY <= 460;

      bool handledAviation = false;
      if (currentTab == AVIATION_TAB_INDEX) {
        if (g_selectedAircraftIndex >= 0) {
          // Card is showing - only the BACK button does anything here.
          bool hitBack = lastTouchX >= 530 && lastTouchX <= 630 &&
                         lastTouchY >= 420 && lastTouchY <= 460;
          if (hitBack) {
            g_selectedAircraftIndex = -1;
          }
          handledAviation = true;
        } else {
          // List is showing - check each row's recorded hit box.
          for (int i = 0; i < g_listRowCount; i++) {
            if (lastTouchY >= g_listRowY0[i] && lastTouchY <= g_listRowY1[i] &&
                lastTouchX >= 530 && lastTouchX <= 780) {
              int aircraftIdx = g_listRowAircraftIdx[i];
              g_selectedAircraftIndex = aircraftIdx;
              aviation_request_detail(g_aircraft[aircraftIdx].icao, g_aircraft[aircraftIdx].callsign);
              handledAviation = true;
              break;
            }
          }
        }
      }

      if (hitNextButton) {
        PanelDisplay::cycleBounceBufferAndRestart();
      } else if (hitPollButton) {
        cycleAviationPollInterval();
      } else if (hitAlertTestButton) {
        g_alertActive = true;
        g_alertShownAtMs = now;
        g_alertColorOverride = colorDanger;
        snprintf(g_alertMessage, sizeof(g_alertMessage), "TEST ALERT - HIDDEN DEBUG TRIGGER");
      } else if (!handledAviation && !g_pageLocked) {
        currentTab = (currentTab + 1) % TAB_COUNT;
      }
    }
  }
  touchWasDown = touched;
}
