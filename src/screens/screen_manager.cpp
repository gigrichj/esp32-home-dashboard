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
#include "../services/spacex_launch_service.h"
#include "../services/aurora_service.h"
#include "../services/wv_astro_service.h"
#include "../services/wv_clearsky_image_service.h"
#include "../services/imagery_service.h"
#include "../services/trips_service.h"
#include "../services/world_trips_banner_service.h"
#include "secrets.h"
#include "../debug_log.h"
#include "../debug_controls.h"
#include <math.h>
#include <time.h>
#include <WiFi.h>
#include "../services/wifi_manager.h"
#include "../state_mutex.h"

using namespace PanelDisplay;

static const char* TAB_NAMES[] = {
  "DASHBOARD", "AVIATION", "ASTRO", "SPACEX", "ISS", "WEATHER", "IMAGERY", "TRENDS", "WV ASTRO", "WORLD TRIPS"
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
static uint16_t colorBgDay, colorSuccessDay, colorDangerDay, colorTextDay, colorDimDay, colorAccentDay, colorStarshipDay;
static uint16_t colorBgNight, colorSuccessNight, colorDangerNight, colorTextNight, colorDimNight, colorAccentNight, colorStarshipNight;
static uint16_t colorStarship;

static bool touchWasDown = false;
static uint32_t touchDownMs = 0;
static const uint32_t TAP_MIN_MS = 50;
static const uint32_t TAP_MAX_MS = 600;
static const uint32_t LONGPRESS_MIN_MS = 900;   // hold longer than this toggles night mode
static uint16_t touchDownX = 0;
static uint16_t touchDownY = 0;
static uint16_t touchMinX = 0;                  // smallest X seen so far this gesture (left excursion)
static uint16_t touchMaxX = 0;                  // largest X seen so far this gesture (right excursion)

// Auto-dim: screen dims (see Canvas::dimFrameBuffer()) between 10pm and
// 8am, unless a bottom-to-top swipe has toggled the override active --
// g_dimOverrideActive forces full brightness until toggled again with
// another bottom-to-top swipe (no timer/auto-revert, unlike the old
// 5-minute wake window this replaced). Automatically reset to false once
// the window ends (see screen_manager_draw()), so each night starts back
// in the normal dimmed state rather than carrying over yesterday's toggle.
static bool g_dimOverrideActive = false;

// Moved up here (was declared much later, right before
// screen_manager_draw()) -- drawHeader(), defined above that point in the
// file, needed to read this too (for the centered LOCKED/BRIGHT-DIMMED
// indicators), which a compiler error caught: "not declared in this
// scope". Updated once per frame in screen_manager_draw(), alongside
// night mode.
static bool g_inDimWindowActive = false;

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
// Alert banner has no auto-timeout -- it stays up until explicitly
// tapped to dismiss (see drawAlertBanner()/touch handler below).
static char g_alertMessage[64] = "";
static uint16_t g_alertColorOverride = 0; // set from colorSuccess/colorDanger at trigger time

static bool g_prevAstroWasGood = false;
static bool g_prevStormWasHigh = false;
static bool g_prevAnyEmergency = false;
static bool g_prevStarshipLaunchToday = false;
static bool g_prevSuperHeavyLaunchToday = false;
static bool g_prevIssGoodPassSoon = false;
static bool g_prevStarshipLaunch30Min = false;
static bool g_prevSuperHeavyLaunch30Min = false;
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

static const int ISS_TAB_INDEX = 4;
static const int ASTRO_TAB_INDEX = 2;

static void drawHeader() {
  screen.fillRect(0, 0, WIDTH, 40, colorAccent);
  screen.setTextSize(2);
  screen.setTextColor(colorBg, colorAccent);
  screen.setTextDatum(textdatum_t::top_left);
  int issTitleWidth = 0; // only set on the ISS page -- used below to center
                         // the LOCKED/BRIGHT-DIMMED indicators in the gap
                         // between this title and the tab indicator, since
                         // this page's title is long enough to otherwise
                         // collide with a fixed banner-center position.
  if (currentTab == 0) {
    // Nudged up from y=12 to y=3 -- makes room for the WiFi status row
    // added below (see the block near the end of this function), moved
    // here from the page body where it was overlapping the cloud
    // graphics. Still comfortably inside this 40px banner.
    screen.drawString("DASHBOARD - LORTON,VA", 10, 3);
  } else if (currentTab == ISS_TAB_INDEX) {
    // Home coordinates alongside the title, same pattern as the
    // Dashboard's "- LORTON,VA" suffix -- useful reference since this
    // page is all about position relative to home.
    char issTitle[56];
    // Parens around the coordinate pair -- without them, a negative
    // longitude right after the "-" separator read ambiguously (looked
    // like two dashes in a row rather than a separator + a negative sign).
    snprintf(issTitle, sizeof(issTitle), "ISS - LORTON,VA (%.4f, %.4f)", (double)HOME_LAT, (double)HOME_LON);
    screen.drawString(issTitle, 10, 12);
    issTitleWidth = (int)strlen(issTitle) * 12; // same 12px/char estimate used elsewhere in this file
  } else if (currentTab == ASTRO_TAB_INDEX) {
    // Shorter than the ISS title (no live coordinates), so it stays well
    // clear of the centered LOCKED/DIMMED indicator without needing the
    // same title-width tracking ISS needed.
    screen.drawString("ASTRO - LORTON, VA", 10, 12);
  } else {
    screen.drawString(TAB_NAMES[currentTab], 10, 12);
  }

  screen.setTextSize(1);
  screen.setTextColor(colorBg, colorAccent);
  screen.setTextDatum(textdatum_t::top_right);
  char tabIndicator[24];
  if (currentTab == 0) {
    // Dashboard has no room for a second line (title + WiFi row already
    // fill the 40px banner), so SWIPE rides on the same line as TAP here.
    snprintf(tabIndicator, sizeof(tabIndicator), "%d/%d  TAP>  <SWIPE", currentTab + 1, TAB_COUNT);
  } else {
    snprintf(tabIndicator, sizeof(tabIndicator), "%d/%d  TAP>", currentTab + 1, TAB_COUNT);
  }

  // Compact WiFi signal icon, flush with the banner's far-right edge --
  // same ascending-bar style and RSSI thresholds as the Dashboard page's
  // WiFi row below, just small enough to fit this 1-line banner strip.
  // Shows all bars unlit rather than hiding the icon when not connected,
  // so its position stays consistent across pages/states. Skipped on the
  // Dashboard page (currentTab 0), which gets its own dedicated WiFi row
  // (SSID + IP, see below) instead of just the bare icon.
  int wifiIconW = 0;
  if (currentTab != 0) {
    int wifiLitBars = 0;
    if (WiFi.status() == WL_CONNECTED) {
      int rssi = g_wifiRssi; // cached on the network task, not read live here
      wifiLitBars = (rssi > -55) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : 1;
    }
    int wifiBarW = 3, wifiGap = 1;
    wifiIconW = wifiBarW * 4 + wifiGap * 3;
    int wifiIconRight = WIDTH - 10; // flush with the banner's existing right margin
    int wifiBaseY = 21; // bars grow upward from this baseline, sized for the 1-line banner
    for (int b = 0; b < 4; b++) {
      int barH = 2 + b * 2; // ascending heights: 2,4,6,8px
      int bx = wifiIconRight - wifiIconW + b * (wifiBarW + wifiGap);
      uint16_t barColor = (b < wifiLitBars) ? colorSuccess : colorDim; // lit bars now green instead of black
      screen.fillRect(bx, wifiBaseY - barH, wifiBarW, barH, barColor);
    }
  }

  // Page number + TAP hint drawn just to the left of the icon (or flush
  // right, same as before, on the Dashboard page where there's no icon).
  // Nudged up on the Dashboard page to match the title above, again to
  // leave room for the new WiFi row below.
  int tabIndicatorX = (wifiIconW > 0) ? (WIDTH - 10 - wifiIconW - 8) : (WIDTH - 10);
  int tabIndicatorY = (currentTab == 0) ? 6 : 15;
  screen.drawString(tabIndicator, tabIndicatorX, tabIndicatorY);

  // <SWIPE hint, second line, genuinely centered under TAP> above it --
  // previously flush to the plain right margin, which visibly drifted
  // out from under TAP> whenever the WiFi icon shifted tabIndicatorX (the
  // two used different right-edges). Skipped on Dashboard, which folded
  // it into the single combined line above instead.
  if (currentTab != 0) {
    // Center specifically under the "TAP>" word, not the whole
    // "X/8  TAP>" string above it -- since that string is right-aligned,
    // "TAP>" only occupies its rightmost portion (4 chars out of 9), so
    // centering under the full string previously pulled <SWIPE too far
    // left. screen.textWidth() matches the Canvas class's own internal
    // width formula exactly, rather than approximating it.
    int tapWordWidth = screen.textWidth("TAP>");
    int swipeCenterX = tabIndicatorX - tapWordWidth / 2;
    screen.setTextDatum(textdatum_t::middle_center);
    screen.drawString("<SWIPE", swipeCenterX, tabIndicatorY + 17);
    screen.setTextDatum(textdatum_t::top_right); // restore -- rest of this function expects it
  }

  // LOCKED / BRIGHT-DIMMED indicators, centered in the banner. On every
  // page except ISS, that's just the banner's horizontal center; on ISS,
  // where the title (with live coordinates) runs long enough to otherwise
  // collide with a fixed center point, it's centered in the actual gap
  // between the title's right edge and the tab indicator instead.
  int indicatorCenterX = WIDTH / 2;
  if (currentTab == ISS_TAB_INDEX) {
    int issTitleRight = 10 + issTitleWidth;
    indicatorCenterX = (issTitleRight + tabIndicatorX) / 2;
  }
  screen.setTextSize(1);
  screen.setTextDatum(textdatum_t::middle_center); // no top_center in this project's textdatum_t
  if (g_pageLocked) {
    screen.setTextColor(colorDanger, colorAccent);
    screen.drawString("LOCKED", indicatorCenterX, 8);
  }
  if (g_inDimWindowActive) {
    screen.setTextColor(g_dimOverrideActive ? colorSuccess : colorBg, colorAccent);
    screen.drawString(g_dimOverrideActive ? "BRIGHT" : "DIMMED", indicatorCenterX, 20);
  }
  screen.setTextDatum(textdatum_t::top_left);

  // Dashboard-only: WiFi network + IP, relocated here from the page body
  // (previously drawn over the cloud background around y=50) -- a second
  // compact row inside this same 40px banner, right-aligned like the tab
  // indicator above it, with a small ascending-bar signal icon to its
  // left in the same style as the per-page icon above.
  if (currentTab == 0) {
    char wifiLine[48];
    uint16_t wifiTextColor;
    int wifiLitBars = 0;
    if (WiFi.status() == WL_CONNECTED) {
      snprintf(wifiLine, sizeof(wifiLine), "%s (%s)", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
      wifiTextColor = colorBg; // matches the title/tap text color above it, per follow-up feedback (was colorSuccess/green)
      int rssi = g_wifiRssi; // cached on the network task, not read live here
      wifiLitBars = (rssi > -55) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : 1;
    } else {
      snprintf(wifiLine, sizeof(wifiLine), "disconnected (%d)", (int)WiFi.status());
      wifiTextColor = colorDanger;
    }
    int wifiRowY = 25; // second row, below the title/tap row above, still inside the 40px banner
    screen.setTextColor(wifiTextColor, colorAccent);
    screen.setTextDatum(textdatum_t::top_right); // explicit rather than relying on leftover state --
                                                  // the centered LOCKED/BRIGHT-DIMMED indicators above
                                                  // reset this to top_left, which sent this text off
                                                  // the right edge of the screen instead of right-aligning it
    screen.drawString(wifiLine, WIDTH - 10, wifiRowY);

    int lineWidth = (int)strlen(wifiLine) * 6; // ~6px/char at text size 1
    int iconBarW = 3, iconGap = 1;
    int iconTotalW = iconBarW * 4 + iconGap * 3;
    int iconRight = WIDTH - 10 - lineWidth - 8; // 8px gap before the text
    int iconBaseY = wifiRowY + 8; // bars grow upward from this baseline

    for (int b = 0; b < 4; b++) {
      int barH = 2 + b * 2; // ascending heights: 2,4,6,8px
      int bx = iconRight - iconTotalW + b * (iconBarW + iconGap);
      uint16_t barColor = (b < wifiLitBars) ? colorSuccess : colorDim;
      screen.fillRect(bx, iconBaseY - barH, iconBarW, barH, barColor);
    }
    screen.setTextColor(colorBg, colorAccent); // restore banner text color for anything drawn after this
  }
}

static void drawCloudIcon(int cx, int cy, int r, uint16_t color); // defined further down
static void drawWeatherBackground(int weatherId, bool isNight, int sunCyOffset = 0, bool showSunIcon = true); // defined further down, used here
static void enqueueAlert(const char* message, uint16_t color); // defined further down
static void advanceAlertQueue(); // defined further down
static bool isStarshipOrSuperHeavy(const String& rocketName); // defined further down
static void drawRocketIcon(int cx, int cy, uint16_t color); // defined further down
static bool isFalconClass(const String& rocketName); // defined further down
static void drawFalconIcon(int cx, int cy, uint16_t color); // defined further down

static void drawDashboardBackground() {
  StateLockGuard lockGuard;
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
    // Sun nudged down ~1/2in (40px, this project's established px-per-
    // half-inch estimate) from its default position, per request.
    drawWeatherBackground(g_weather.weatherId, isNight, 40, true);
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

  // A rocket silhouette crossing on its own lane, opposite direction from
  // the plane below it -- same "ambient nod to a real page" treatment as
  // the plane/ISS/galaxy above, this time for the SpaceX page. Nose
  // points right (direction of travel), with a small flickering exhaust
  // flame trailing behind so it reads as "launching/flying" rather than
  // just drifting like the plane.
  {
    uint16_t rocketColor = screen.color565(200, 200, 210);
    uint16_t flameColorA = screen.color565(255, 150, 60);
    uint16_t flameColorB = screen.color565(255, 210, 90);
    int span = WIDTH + 80;
    int x = (int)((t / 22) % (uint32_t)span) - 40;
    int y = 340;

    // Fuselage + nose cone, pointing right.
    screen.fillRect(x - 14, y - 3, 20, 6, rocketColor);
    screen.fillTriangle(x + 6, y - 5, x + 6, y + 5, x + 16, y, rocketColor);

    // Fins near the tail.
    screen.fillTriangle(x - 14, y - 3, x - 20, y - 9, x - 14, y, rocketColor);
    screen.fillTriangle(x - 14, y + 3, x - 20, y + 9, x - 14, y, rocketColor);

    // Flickering exhaust flame -- alternates color each ~150ms so it
    // reads as active thrust rather than a static decoration.
    uint16_t flameColor = ((t / 150) % 2 == 0) ? flameColorA : flameColorB;
    screen.fillTriangle(x - 14, y - 2, x - 14, y + 2, x - 22, y, flameColor);
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
  StateLockGuard lockGuard;
  drawDashboardBackground();
  astro_recompute_moon_phase(); // keeps moon illum% current for the ASTRO column below

  screen.setTextSize(3);
  screen.setTextColor(colorAccent, colorBg);
  screen.setTextDatum(textdatum_t::top_left);
  screen.drawString(formatCurrentDateTime(), 20, 55);
  screen.setTextColor(colorText, colorBg);

  // WiFi status moved into the banner in drawHeader() -- see the
  // Dashboard-only block there. This used to draw here, overlapping the
  // cloud background.
  screen.setTextDatum(textdatum_t::top_left);
  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);

  int leftX = 20;
  int rightX = 420;
  char line[64];

  // WEATHER + AIR QUALITY column
  int y = 147; // moved up ~1/2in (was 207) -- LAUNCHES section below was
               // overflowing past the bottom of the screen
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

  // LAUNCHES: next upcoming SpaceX launch, same cross-page-teaser
  // pattern as the Sunset/Sunrise and ISS "next pass" lines above.
  {
    screen.setTextSize(2);
    screen.setTextColor(colorAccent, colorBg);
    screen.drawString("LAUNCHES", leftX, y);
    screen.drawLine(leftX, y + 20, leftX + 110, y + 20, colorAccent);
    y += 30;

    if (g_spacexValid && g_spacexLaunchCount > 0) {
      SpacexLaunch& next = g_spacexLaunches[0];
      // Countdown format ("in Xh Ym"), matching the Sunset/Sunrise and
      // ISS "next pass" teasers elsewhere on this page, instead of an
      // absolute date/time -- consistent with the rest of the Dashboard.
      uint32_t nowUnixL = (uint32_t)time(nullptr);
      uint32_t secsUntilLaunch = (next.netUnix > nowUnixL) ? (next.netUnix - nowUnixL) : 0;
      int launchHH = secsUntilLaunch / 3600;
      int launchMM = (secsUntilLaunch % 3600) / 60;

      // Highlighted (success color) if launching within 24h, same
      // "stands out at a glance" treatment as "ISS visible now!" above.
      bool launchingSoon = secsUntilLaunch <= 24UL * 3600UL;
      bool isStarship = isStarshipOrSuperHeavy(next.rocketName);

      if (isStarship) {
        // Starship/Super Heavy gets its own icon + bigger, distinctly
        // colored treatment -- SpaceX's next-gen vehicle stands out from
        // routine Falcon 9 flights at a glance.
        drawRocketIcon(leftX, y - 4, colorStarship);
        screen.setTextSize(3);
        screen.setTextColor(colorStarship, colorBg);
        screen.drawString(next.rocketName.c_str(), leftX + 28, y);
        y += 42; // more breathing room before the countdown line below (was 34)

        screen.setTextSize(2);
        screen.setTextColor(colorDim, colorBg);
        char countdownLine[32];
        snprintf(countdownLine, sizeof(countdownLine), "in %dh %dm", launchHH, launchMM);
        screen.drawString(countdownLine, leftX, y);
        y += 30;
      } else {
        screen.setTextColor(launchingSoon ? colorSuccess : colorText, colorBg);
        char launchLine1[48];
        snprintf(launchLine1, sizeof(launchLine1), "Next: %s in %dh %dm",
                 next.rocketName.c_str(), launchHH, launchMM);
        screen.drawString(launchLine1, leftX, y);
        y += 30;
      }

      screen.setTextColor(colorDim, colorBg);
      screen.drawString(next.missionName.c_str(), leftX, y);

      // Surface whatever Launch Library 2 status comes back (Go, TBD,
      // Hold, In Flight, Success, Failure, etc.) as the mission
      // progresses -- previously only visible on the SpaceX page itself
      // via next.statusName. Same color convention used there.
      if (next.statusName.length() > 0) {
        y += 27; // nudged 3px lower per follow-up feedback (was +24)
        uint16_t dashStatusColor = colorText;
        if (next.statusName.equalsIgnoreCase("Go") || next.statusName.equalsIgnoreCase("Success")) {
          dashStatusColor = colorSuccess;
        } else if (next.statusName.equalsIgnoreCase("TBD") || next.statusName.equalsIgnoreCase("Hold")) {
          dashStatusColor = colorDim;
        } else if (next.statusName.equalsIgnoreCase("Failure")) {
          dashStatusColor = colorDanger;
        }
        screen.setTextColor(dashStatusColor, colorBg);
        String upperStatus = next.statusName;
        upperStatus.toUpperCase();
        screen.drawString(upperStatus.c_str(), leftX, y);
      }
    } else {
      screen.setTextColor(colorDim, colorBg);
      screen.drawString("No launches in 30 days", leftX, y);
    }
  }
  y += 40;

  // AIRCRAFT + ISS column -- the second of 2 even columns.
  // Aligned with the WEATHER column's start (y=147, was 207) so both
  // columns begin flush at the top, per follow-up feedback.
  int y2 = 147;
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
  y2 += 34; // BUG FIX: this increment was missing, causing the Aurora line
            // below to draw on the exact same row as "Next pass in Xh Ym"
            // (or "ISS visible now!") -- both overlapped character-for-
            // character, which is the stray grey print seen behind
            // "UNLIKELY" on-device.
  {
    // Aurora/Kp teaser, added under OVERHEAD below the Next Pass line --
    // same data/color/threshold convention as the WV_ASTRO page's
    // "AURORA (Kp X.X): LABEL" line (aurora_visibility_label(), same
    // Likely/Possible/Slight Chance/Unlikely color ramp), just condensed
    // to fit this column's width.
    char auroraPrefix[32];
    const char* auroraLabel = nullptr;
    uint16_t auroraLabelColor = colorText;
    if (g_kpObservedValid) {
      snprintf(auroraPrefix, sizeof(auroraPrefix), "Aurora (Kp %.1f): ", (double)g_currentKp);
      auroraLabel = aurora_visibility_label(g_currentKp);
      if (strcmp(auroraLabel, "Likely") == 0) auroraLabelColor = colorSuccess;
      else if (strcmp(auroraLabel, "Possible") == 0) auroraLabelColor = screen.color565(230, 200, 40);
      else if (strcmp(auroraLabel, "Slight Chance") == 0) auroraLabelColor = screen.color565(230, 130, 40);
      else auroraLabelColor = colorDanger;
    } else {
      snprintf(auroraPrefix, sizeof(auroraPrefix), "Aurora: no data yet");
    }
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString(auroraPrefix, rightX, y2);
    if (auroraLabel != nullptr) {
      int labelX = rightX + screen.textWidth(auroraPrefix);
      screen.setTextColor(auroraLabelColor, colorBg);
      screen.drawString(auroraLabel, labelX, y2);
    }
  }
  y2 += 34;

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
// Matches the fetch radius (AVIATION_RANGE_NM in aviation_service.cpp,
// reduced 40nm -> 20nm). Ring spacing, outer range label, and the
// visible/aircraft filtering below all derive from this, so nothing
// else needs to change.
static const float RADAR_MAX_RANGE_NM = 20.0f;

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
  StateLockGuard lockGuard;
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
  StateLockGuard lockGuard;
  // Diagnostic placeholder removed -- restored alongside
  // aviation_service.cpp's fetch functions now that the mutex refactor
  // has closed the underlying data race. See aviation_service_update()'s
  // comment for the full story.
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
static void drawWeatherBackground(int weatherId, bool isNight, int sunCyOffset, bool showSunIcon) {
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
    } else if (showSunIcon) {
      // Clear day: gently pulsing sun with rays, tucked in the corner.
      // sunCyOffset lets individual pages nudge its vertical position
      // (the Dashboard moves it down ~1/2in to clear other elements)
      // without affecting other callers; showSunIcon lets a page that
      // already shows its own foreground sun icon (the Weather page)
      // skip this faded background one entirely.
      uint16_t sunColor = screen.color565(50, 45, 25);
      int cx = WIDTH - 90, cy = 80 + sunCyOffset;
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
  StateLockGuard lockGuard;
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
  // The Weather page already shows its own bright sun icon (below, via
  // drawWeatherIcon) top-left -- the faded background sun in the corner
  // was redundant/distracting here, so it's suppressed on this page only.
  drawWeatherBackground(g_weather.weatherId, isNight, 0, false);

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

  {
    // 24-hour precipitation-probability strip, from the same Open-Meteo
    // hourly data source already used for the UV index (see
    // fetchHourlyPrecip() in weather_service.cpp) -- gives a glance at
    // rain chances through the day/night ahead, versus the single
    // "right now" PRECIP gauge drawn above.
    //
    // Follow-up: dropped the separate "24HR PRECIP" title row + its
    // underline that used to sit above the box -- the label now lives
    // inline, bottom-right of the box itself (same row as the hour tick
    // labels). That reclaims the ~20px the title row used to take,
    // handed straight to barAreaH so the bars read much more clearly.
    int stripX = 20, precipTopY = 352, precipStripW = WIDTH - 40;
    screen.setTextSize(2);
    screen.setTextColor(colorAccent, colorBg);
    screen.setTextDatum(textdatum_t::top_left);

    int barAreaY = precipTopY;
    int barAreaH = 37; // was 17 -- grew by the ~20px freed from dropping the title row
    int barBaselineY = barAreaY + barAreaH;
    // The end-caps hang down from this top (baseline) line only, meeting
    // the forecast-row divider line further below (drawn at stripY - 14,
    // where stripY = 425 -- kept as a literal here since stripY isn't
    // declared until after this block).
    int frameBottomY = 411;

    if (g_precipHourlyValid && g_precipHourlyCount > 0) {
      int n = g_precipHourlyCount;
      int slotW = precipStripW / n;
      // Integer division truncates (e.g. 760/24 = 31, not 31.67), so the
      // actual bars only span slotW*n px -- less than the nominal
      // precipStripW. The border/label used to be positioned against the
      // nominal width, leaving a gap between the last bar and the right
      // border. usedWidth is the real, exact width the bars occupy, and
      // everything below (border, label) now aligns to that instead.
      int usedWidth = slotW * n;
      int rightEdgeX = stripX + usedWidth;
      uint16_t barColor = screen.color565(70, 150, 220);

      // Faint background tint across the whole bar area so 0%-chance
      // hours read as "checked, nothing expected" rather than looking
      // identical to a strip that never loaded. Drawn first so bars/
      // baseline/labels all sit on top of it.
      uint16_t tintColor = screen.color565(30, 40, 55);
      screen.fillRect(stripX + 1, barAreaY, usedWidth - 2, barAreaH, tintColor);

      // Any nonzero chance gets at least this many pixels -- previously
      // a low prob (5-10%) could round down to 0px against the 17px-tall
      // bar area and vanish entirely against the flat baseline.
      static const int MIN_VISIBLE_BAR_PX = 2;
      for (int i = 0; i < n; i++) {
        int prob = constrain(g_precipHourly[i].precipProb, 0, 100);
        int barH = (int)(barAreaH * (prob / 100.0f));
        if (prob > 0 && barH < MIN_VISIBLE_BAR_PX) barH = MIN_VISIBLE_BAR_PX;
        int bx = stripX + i * slotW;
        if (barH > 0) {
          screen.fillRect(bx + 1, barBaselineY - barH, max(slotW - 2, 1), barH, barColor);
        }
      }
      screen.drawLine(stripX, barBaselineY, rightEdgeX, barBaselineY, colorDim);
      screen.drawLine(stripX, barBaselineY, stripX, frameBottomY, colorDim);
      screen.drawLine(rightEdgeX, barBaselineY, rightEdgeX, frameBottomY, colorDim);

      // Compact hour-of-day tick labels every 4 hours -- labeling all 24
      // would be too dense at this width and this font's minimum size.
      screen.setTextSize(1);
      screen.setTextColor(colorDim, colorBg);
      for (int i = 0; i < n; i += 4) {
        time_t t = (time_t)g_precipHourly[i].unixTime;
        struct tm* ti = localtime(&t);
        int h12 = ti->tm_hour % 12;
        if (h12 == 0) h12 = 12;
        char hourBuf[8];
        snprintf(hourBuf, sizeof(hourBuf), "%d%s", h12, ti->tm_hour < 12 ? "A" : "P");
        // The leftmost label sits flush against the end-cap line -- nudge
        // it 1px right so it doesn't crowd/touch the border.
        int lx = stripX + i * slotW + (i == 0 ? 1 : 0);
        screen.drawString(hourBuf, lx, barBaselineY + 3);
      }

      // Relocated label -- was a separate title+underline above the box,
      // now sits bottom-right, same row/size as the hour ticks. Anchored
      // to the corrected rightEdgeX (not the old nominal-width position),
      // and nudged an additional 10px (1/8in, this project's established
      // 80px/in scale) further left per follow-up feedback.
      screen.setTextColor(colorAccent, colorBg);
      int labelW = screen.textWidth("24HR PRECIP");
      screen.drawString("24HR PRECIP", rightEdgeX - labelW - 12, barBaselineY + 9);
    } else {
      screen.setTextSize(2);
      screen.setTextColor(colorDim, colorBg);
      char errLine[32];
      snprintf(errLine, sizeof(errLine), "HTTP %d", g_precipHourlyLastHttpCode);
      screen.drawString(errLine, stripX, barAreaY);
    }
    screen.setTextSize(2);
    screen.setTextColor(colorText, colorBg);
  }

  int stripY = 425;
  // Raised 3px (was stripY - 11) per follow-up feedback -- forecast
  // icons/text below are unaffected since they're still positioned off
  // of stripY itself, not this divider line's offset.
  screen.drawLine(20, stripY - 14, WIDTH - 20, stripY - 14, colorDim);

  int colW = (WIDTH - 40) / 5;
  screen.setTextDatum(textdatum_t::middle_center);
  if (g_forecastCount == 0) {
    // 5-day strip failed to load -- show the HTTP result instead of
    // silently rendering nothing, same convention used elsewhere on this
    // page (precip strip, UV, air quality) via a "HTTP %d" fallback.
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    char forecastErrLine[24];
    snprintf(forecastErrLine, sizeof(forecastErrLine), "HTTP %d", g_forecastLastHttpCode);
    screen.drawString(forecastErrLine, WIDTH / 2, stripY + 24);
    screen.setTextColor(colorText, colorBg);
  }
  for (int i = 0; i < g_forecastCount; i++) {
    int cx = 20 + colW * i + colW / 2;

    screen.setTextSize(2);
    screen.setTextColor(colorText, colorBg);
    screen.drawString(g_forecast[i].dayLabel, cx, stripY + 2);

    drawWeatherIcon(cx, stripY + 24, 12, g_forecast[i].weatherId, colorText);

    // Font has no "%" glyph -- "R" suffix instead, same convention as
    // "pct" used elsewhere in this project for percentages. Omitted
    // entirely on dry days rather than cluttering the strip with "0R".
    if (g_forecast[i].precipChance > 0) {
      char tempPart[16];
      char rainPart[16];
      snprintf(tempPart, sizeof(tempPart), "%.0f/%.0f ", g_forecast[i].highF, g_forecast[i].lowF);
      snprintf(rainPart, sizeof(rainPart), "%dR", g_forecast[i].precipChance);

      // drawString() only supports top_left and middle_center datums (no
      // left+vcenter option), so instead of left-aligning we compute each
      // piece's own center point and draw both with middle_center -- same
      // vertical position, x-centers placed so the pair reads as one string.
      int tempW = screen.textWidth(tempPart);
      int rainW = screen.textWidth(rainPart);
      int totalW = tempW + rainW;
      int leftEdge = cx - totalW / 2;
      int tempCx = leftEdge + tempW / 2;
      int rainCx = leftEdge + tempW + rainW / 2;

      screen.setTextColor(colorText, colorBg);
      screen.drawString(tempPart, tempCx, stripY + 44);

      screen.setTextColor(colorAccent, colorBg);
      screen.drawString(rainPart, rainCx, stripY + 44);

      screen.setTextColor(colorText, colorBg);
    } else {
      char hilo[32];
      snprintf(hilo, sizeof(hilo), "%.0f / %.0f", g_forecast[i].highF, g_forecast[i].lowF);
      screen.drawString(hilo, cx, stripY + 44);
    }
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
  StateLockGuard lockGuard;
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
    // Past segment (before "now") drawn dim, future segment drawn at
    // full brightness, so it's visually clear which part of the line is
    // where the ISS has already been versus where it's headed.
    uint16_t colorTrackPast = screen.color565(120, 85, 30);
    uint16_t colorTrackFuture = screen.color565(255, 170, 60);
    int prevX = 0, prevY = 0;
    bool havePrev = false;
    for (int i = 0; i < g_issTrackCount; i++) {
      int px = MAP_X + (int)((g_issTrack[i].lon + 180) / 360.0f * MAP_W);
      int py = MAP_Y + (int)((90 - g_issTrack[i].lat) / 180.0f * MAP_H);
      if (havePrev && abs(px - prevX) < MAP_W / 2) {
        uint16_t segColor = (i <= ISS_TRACK_NOW_INDEX) ? colorTrackPast : colorTrackFuture;
        screen.drawLine(prevX, prevY, px, py, segColor);
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
  // Same parens treatment as the header -- makes a negative longitude
  // read unambiguously rather than looking like a stray dash.
  snprintf(posLabel, sizeof(posLabel), "(%.2f, %.2f)", g_iss.lat, g_iss.lon);
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
    screen.drawString(row, col2X + 104, contentY + 32); // nudged 4px right per follow-up feedback
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
    screen.drawString(row, col2X + 104, detailY); // nudged 4px right per follow-up feedback

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
        // Low-elevation passes are barely visible from the ground (often
        // lost behind horizon haze/trees) -- flag them so it's obvious at
        // a glance which passes are actually worth going outside for.
        bool isLowPass = p.maxElevationDeg < 10;
        if (isLowPass) {
          snprintf(line, sizeof(line), "%-6s%-7sEl%-3d(low)",
                   passDateBuf,
                   passTimeBuf,
                   p.maxElevationDeg);
        } else {
          snprintf(line, sizeof(line), "%-6s%-7sEl%-3d",
                   passDateBuf,
                   passTimeBuf,
                   p.maxElevationDeg);
        }
        screen.setTextColor(isLowPass ? colorDim : colorText, colorBg);
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
  // Moved from center (WIDTH/2) to far left -- frees the entire right side
  // of the page for the legend, diagnostics text, and the easter-egg image
  // with zero geometric overlap (the whole badge shape stays within
  // roughly x:40-300 at this cx, well clear of anything on the right).
  int cx = 170;
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
  int rssi = g_wifiRssi; // cached on the network task, not read live here

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
// (see the metrics computed at the top of drawDebugBadge()). Placed at
// the far top-right, clear of the badge (now moved far left) at every
// height on this page.
static void drawDebugLegend() {
  uint16_t planeColor = screen.color565(120, 200, 150);
  uint16_t issColor = screen.color565(150, 170, 235);
  uint16_t galaxyColor = screen.color565(170, 120, 210);

  int legX = 520;
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

static void draw_imagery() {
  // Formerly the DEBUG page -- badge, legend, and crash diagnostics all
  // removed, this is now a pure image gallery. Image rotates randomly
  // every 15 minutes (see imagery_service.cpp), sized to 600x330 (75% of
  // the 800x440 area below the top banner) and centered within it.
  StateLockGuard lockGuard;
  if (g_imageryValid && g_imageryPixels != nullptr) {
    int imgX = (WIDTH - g_imageryWidth) / 2;
    int imgY = 40 + (HEIGHT - 40 - g_imageryHeight) / 2;
    screen.drawRGBBitmap(imgX, imgY, g_imageryPixels, g_imageryWidth, g_imageryHeight);
  }
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
  StateLockGuard lockGuard;
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
      // Small Bortle gradient bar (1-9 dark-sky scale) with a pointer,
      // so the number means something at a glance instead of requiring
      // the viewer to already know the scale. Green (dark) -> red (bright
      // city sky), same multi-stop gradient approach used for AQI/UV on
      // the Weather page.
      static const uint8_t bortleStops[5][3] = {
        {80, 200, 120}, {160, 200, 60}, {230, 200, 40}, {230, 130, 40}, {220, 60, 60}
      };
      int barX = 560, barY = 78, barW = 132, barH = 8;
      for (int px = 0; px < barW; px += 2) {
        float frac = (float)px / (float)(barW - 1);
        screen.fillRect(barX + px, barY, 2, barH, multiStopGradient(frac, bortleStops, 5));
      }
      float bortleFrac = constrain((HOME_BORTLE_CLASS - 1.0f) / 8.0f, 0.0f, 1.0f);
      int bortlePointerX = barX + (int)(bortleFrac * (barW - 1));
      screen.fillTriangle(bortlePointerX - 4, barY - 5, bortlePointerX + 4, barY - 5, bortlePointerX, barY - 1, colorText);
    }

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
        // Middle column, same row as the two titles either side of it --
        // was at y=79 before, directly overlapping the new Bortle
        // gradient bar added just above.
        screen.drawString(bestLine, 290, 55);
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
  // New-moon countdown folded right into the title, in parens, instead
  // of a separate line below -- more directly useful for planning a
  // dark-sky shoot than the illumination % alone, without needing extra
  // vertical space.
  char moonTitle[40];
  snprintf(moonTitle, sizeof(moonTitle), "MOON (New moon in %.0f days)", g_daysUntilNewMoon);
  screen.drawString(moonTitle, col1X, row2Y);
  int moonTitleWidth = (int)strlen(moonTitle) * 12;
  screen.drawLine(col1X, row2Y + 20, col1X + moonTitleWidth, row2Y + 20, colorAccent);

  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);
  screen.drawString(g_moonPhaseLabel, col1X, row2Y + 34);
  char moonPct[24];
  snprintf(moonPct, sizeof(moonPct), "%.0f pct illuminated", g_moonIllumPercent);
  screen.setTextSize(2);
  screen.setTextColor(colorDim, colorBg);
  screen.drawString(moonPct, col1X, row2Y + 62);

  {
    int moonCx = col1X + 393, moonCy = row2Y + 40, moonR = 32; // shifted right ~1/2in (40px) --
                                                                 // was overlapping the MOON text block.
    screen.fillCircle(moonCx, moonCy, moonR, screen.color565(230, 230, 210));
    float shadowFrac = g_moonPhaseFraction;
    bool waxing = shadowFrac < 0.5f;
    float distFromFull = fabsf(shadowFrac - 0.5f) * 2.0f; // 0 at full, 1 at new
    // BUG FIX: the shadow-overlap amount was previously inverted -- it
    // shrank toward "barely touching the disc" near BOTH new moon and
    // full moon, so the moon rendered as nearly fully lit at both
    // extremes instead of nearly fully dark at new moon. This overlap
    // should shrink to 0 (shadow center = moon center, fully covered) as
    // distFromFull -> 1 (new moon), and grow to 2*moonR (shadow moved
    // completely off the disc, fully lit) as distFromFull -> 0 (full
    // moon), matching the "X pct illuminated" text alongside it.
    // Small safety margin (+2px) added to the max offset so the shadow
    // circle never sits at EXACT geometric tangency with the moon
    // circle at full moon (distFromFull=0) -- two circles of the same
    // radius with centers exactly 2*radius apart are mathematically
    // tangent (touching at one point), but pixel rasterization isn't
    // perfectly precise at that exact boundary and can leak a pixel or
    // two into the visible circle right at the tangent point -- the
    // small dent seen on a real full moon. The +2px margin gives a
    // clean gap instead, eliminating the edge case entirely.
    float offsetFromCenter = (1.0f - distFromFull) * (2.0f * moonR + 2.0f);
    int shadowCx = waxing ? (int)(moonCx - offsetFromCenter) : (int)(moonCx + offsetFromCenter);
    screen.fillCircle(shadowCx, moonCy, moonR, colorBg);
  }

  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString("STORM RISK", col3X, row2Y);
  screen.drawLine(col3X, row2Y + 20, col3X + 132, row2Y + 20, colorAccent);
  if (tonightIdx >= 0) {
    int li = g_astroForecast[tonightIdx].liftedindex;
    screen.setTextSize(2);
    // Match the 4-tier legend colors below instead of the old
    // Stable/everything-else binary coloring -- thresholds mirror
    // astro_instability_label() exactly (astro_seeing_service.cpp).
    uint16_t stormValueColor;
    if (li > 0) {
      stormValueColor = colorSuccess;
    } else if (li > -4) {
      stormValueColor = screen.color565(230, 200, 40);
    } else if (li > -8) {
      stormValueColor = screen.color565(230, 130, 40);
    } else {
      stormValueColor = colorDanger;
    }
    screen.setTextColor(stormValueColor, colorBg);
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
                            bool (*getValue)(int idx, float* outValue), uint16_t lineColor,
                            const char* unit = "") {
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
  char nowLabel[20];
  if (unit[0] != '\0') {
    snprintf(nowLabel, sizeof(nowLabel), "%.0f %s", lastValidValue, unit);
  } else {
    snprintf(nowLabel, sizeof(nowLabel), "%.0f", lastValidValue);
  }
  screen.drawString(nowLabel, x + w - 4, y + 2);
  screen.setTextDatum(textdatum_t::top_left);
}

// Two overlaid series in one panel, each independently normalized to fill
// the same plot height -- AQI and UV Index have very different numeric
// ranges (AQI can run 0-500+, UV Index 0-11), so sharing one y-scale
// would flatten one of them into invisibility. Two titles side by side,
// each with a matching-color underline; combined "AQI/UVI" current-value
// label, right-aligned, three colored segments since a single drawString
// call can't mix colors -- a missing series shows "-" in its slot so the
// "/" separator position stays fixed.
static void drawDualTrendPanel(int x, int y, int w, int h,
                                const char* title1, bool (*getValue1)(int idx, float* outValue), uint16_t color1,
                                const char* title2, bool (*getValue2)(int idx, float* outValue), uint16_t color2) {
  screen.setTextSize(2);
  screen.setTextDatum(textdatum_t::top_left);

  screen.setTextColor(color1, colorBg);
  screen.drawString(title1, x, y);
  int title1Width = (int)strlen(title1) * 12;
  screen.drawLine(x, y + 20, x + title1Width, y + 20, color1);

  int title2X = x + title1Width + 14;
  screen.setTextColor(color2, colorBg);
  screen.drawString(title2, title2X, y);
  int title2Width = (int)strlen(title2) * 12;
  screen.drawLine(title2X, y + 20, title2X + title2Width, y + 20, color2);

  int plotY = y + 30;
  int plotH = h - 30;
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

  int oldestIdx = (g_trendSampleCount < TREND_MAX_SAMPLES) ? 0 : g_trendNextWriteIdx;

  // Shared scale across both series -- AQI (OpenWeatherMap's 1-5 index)
  // and UV Index (roughly 0-11) are genuinely comparable ranges, unlike
  // the earlier independent-per-series normalization this replaced (which
  // made two flat-ish lines land on the exact same pixels, so the second
  // one drawn completely hid the first).
  float minV = 1e9f, maxV = -1e9f; bool any1 = false, any2 = false;
  for (int i = 0; i < g_trendSampleCount; i++) {
    int idx = (oldestIdx + i) % TREND_MAX_SAMPLES;
    float v;
    if (getValue1(idx, &v)) { if (v < minV) minV = v; if (v > maxV) maxV = v; any1 = true; }
    if (getValue2(idx, &v)) { if (v < minV) minV = v; if (v > maxV) maxV = v; any2 = true; }
  }

  if (!any1 && !any2) {
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.setTextDatum(textdatum_t::middle_center);
    screen.drawString("No data yet", x + w / 2, plotY + plotH / 2);
    screen.setTextDatum(textdatum_t::top_left);
    return;
  }
  if (maxV - minV < 0.001f) { minV -= 1.0f; maxV += 1.0f; }

  float lastValid1 = 0, lastValid2 = 0;

  if (any1) {
    int prevPx = 0, prevPy = 0; bool havePrev = false;
    for (int i = 0; i < g_trendSampleCount; i++) {
      int idx = (oldestIdx + i) % TREND_MAX_SAMPLES;
      float v;
      if (!getValue1(idx, &v)) { havePrev = false; continue; }
      lastValid1 = v;
      int px = x + (int)((float)i / (float)(g_trendSampleCount - 1) * (w - 1));
      float frac = (v - minV) / (maxV - minV);
      int py = plotY + plotH - 1 - (int)(frac * (plotH - 1));
      if (havePrev) screen.drawLine(prevPx, prevPy, px, py, color1);
      prevPx = px; prevPy = py; havePrev = true;
    }
  }

  if (any2) {
    int prevPx = 0, prevPy = 0; bool havePrev = false;
    for (int i = 0; i < g_trendSampleCount; i++) {
      int idx = (oldestIdx + i) % TREND_MAX_SAMPLES;
      float v;
      if (!getValue2(idx, &v)) { havePrev = false; continue; }
      lastValid2 = v;
      int px = x + (int)((float)i / (float)(g_trendSampleCount - 1) * (w - 1));
      float frac = (v - minV) / (maxV - minV);
      int py = plotY + plotH - 1 - (int)(frac * (plotH - 1));
      if (havePrev) screen.drawLine(prevPx, prevPy, px, py, color2);
      prevPx = px; prevPy = py; havePrev = true;
    }
  }

  screen.setTextSize(1);
  screen.setTextDatum(textdatum_t::top_left);
  char label1[8], label2[8];
  if (any1) snprintf(label1, sizeof(label1), "%.0f", lastValid1); else snprintf(label1, sizeof(label1), "-");
  if (any2) snprintf(label2, sizeof(label2), "%.0f", lastValid2); else snprintf(label2, sizeof(label2), "-");
  const char* sep = " / "; // spaces on both sides, per follow-up feedback
  int w1 = (int)strlen(label1) * 6;
  int wSep = (int)strlen(sep) * 6;
  int w2 = (int)strlen(label2) * 6;
  int startX = (x + w - 4) - (w1 + wSep + w2);
  screen.setTextColor(color1, colorBg);
  screen.drawString(label1, startX, y + 2);
  screen.setTextColor(colorText, colorBg);
  screen.drawString(sep, startX + w1, y + 2);
  screen.setTextColor(color2, colorBg);
  screen.drawString(label2, startX + w1 + wSep, y + 2);
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
static bool trendGetUvIndex(int idx, float* outValue) {
  if (g_trendSamples[idx].uvIndex < 0) return false;
  *outValue = g_trendSamples[idx].uvIndex;
  return true;
}
static bool trendGetAircraft(int idx, float* outValue) {
  *outValue = (float)g_trendSamples[idx].aircraftCount;
  return true; // 0 is a legitimate value here (genuinely no aircraft nearby)
}
static bool trendGetAstro(int idx, float* outValue) {
  if (g_trendSamples[idx].astroBadness < 0) return false;
  *outValue = (1.0f - g_trendSamples[idx].astroBadness) * 100.0f; // inverted to a quality score, 0..100, higher is better
  return true;
}

// Fetch-health "age" trackers -- minutes since each fetch type last
// succeeded (see trend_history_service.cpp). -1 sentinel (never
// succeeded yet) matches the astroBadness convention above.
static bool trendGetWeatherAge(int idx, float* outValue) {
  if (g_trendSamples[idx].weatherStaleMin < 0) return false;
  *outValue = g_trendSamples[idx].weatherStaleMin;
  return true;
}
static bool trendGetAirQualityAge(int idx, float* outValue) {
  if (g_trendSamples[idx].airQualityStaleMin < 0) return false;
  *outValue = g_trendSamples[idx].airQualityStaleMin;
  return true;
}
static bool trendGetAstroAge(int idx, float* outValue) {
  if (g_trendSamples[idx].astroStaleMin < 0) return false;
  *outValue = g_trendSamples[idx].astroStaleMin;
  return true;
}
static bool trendGetPrecipAge(int idx, float* outValue) {
  if (g_trendSamples[idx].precipStaleMin < 0) return false;
  *outValue = g_trendSamples[idx].precipStaleMin;
  return true;
}
static bool trendGetAviationAge(int idx, float* outValue) {
  if (g_trendSamples[idx].aviationStaleMin < 0) return false;
  *outValue = g_trendSamples[idx].aviationStaleMin;
  return true;
}
static bool trendGetIssAge(int idx, float* outValue) {
  if (g_trendSamples[idx].issStaleMin < 0) return false;
  *outValue = g_trendSamples[idx].issStaleMin;
  return true;
}
static bool trendGetSpacexAge(int idx, float* outValue) {
  if (g_trendSamples[idx].spacexStaleMin < 0) return false;
  *outValue = g_trendSamples[idx].spacexStaleMin;
  return true;
}
static bool trendGetSpacexDetailAge(int idx, float* outValue) {
  if (g_trendSamples[idx].spacexDetailStaleMin < 0) return false;
  *outValue = g_trendSamples[idx].spacexDetailStaleMin;
  return true;
}

static void draw_trends() {
  StateLockGuard lockGuard;
  screen.setTextDatum(textdatum_t::top_left);

  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);
  char header[64];
  // Two distinct things shown side by side: real device uptime (down to
  // the minute, from millis()), and how much history the trend graphs
  // below actually cover (samples * 5min interval -- naturally caps at
  // 24h once the ring buffer fills, since older samples roll off). These
  // two only diverge once uptime exceeds 24h; until then they track
  // together since a reboot/crash resets both at once.
  uint32_t upTotalMin = millis() / 60000UL;
  uint32_t upHours = upTotalMin / 60;
  uint32_t upMins = upTotalMin % 60;
  int hoursCovered = (g_trendSampleCount * (int)(TREND_SAMPLE_INTERVAL_MS / 1000)) / 3600;
  // No tilde -- this custom bitmap font's charset doesn't include "~"
  // (renders as a placeholder glyph, looked like a stray question mark).
  snprintf(header, sizeof(header), "Up %lu hrs %lu min / Data: %d samples (%d hrs)",
           (unsigned long)upHours, (unsigned long)upMins, g_trendSampleCount, hoursCovered);
  screen.drawString(header, 20, 50);

  // 4x3 uniform grid -- the 4 original headline metrics (row 1, unchanged
  // in meaning, just resized) plus 8 new fetch-health "age" trackers
  // (rows 2-3, minutes since each fetch type last succeeded). Titles kept
  // short to fit this panel width at textSize(2) (~12px/char).
  int panelW = 178, panelH = 115;
  int gap = 15;
  int col0X = 20, col1X = col0X + panelW + gap, col2X = col1X + panelW + gap, col3X = col2X + panelW + gap;
  int row1Y = 85, row2Y = row1Y + panelH + gap, row3Y = row2Y + panelH + gap;

  drawTrendPanel(col0X, row1Y, panelW, panelH, "TEMP (F)", trendGetTemp, colorAccent, "F");
  drawDualTrendPanel(col1X, row1Y, panelW, panelH,
                      "AQI", trendGetAqi, screen.color565(60, 120, 255),
                      "UVI", trendGetUvIndex, screen.color565(230, 60, 60));
  drawTrendPanel(col2X, row1Y, panelW, panelH, "AIRCRAFT", trendGetAircraft, screen.color565(90, 200, 255));
  drawTrendPanel(col3X, row1Y, panelW, panelH, "ASTRO QLTY", trendGetAstro, screen.color565(170, 120, 210));

  drawTrendPanel(col0X, row2Y, panelW, panelH, "WEATHER AGE", trendGetWeatherAge, screen.color565(100, 180, 255), "MIN");
  drawTrendPanel(col1X, row2Y, panelW, panelH, "AQ AGE", trendGetAirQualityAge, screen.color565(200, 160, 60), "MIN");
  drawTrendPanel(col2X, row2Y, panelW, panelH, "ASTRO AGE", trendGetAstroAge, screen.color565(140, 100, 190), "MIN");
  drawTrendPanel(col3X, row2Y, panelW, panelH, "PRECIP AGE", trendGetPrecipAge, screen.color565(80, 160, 220), "MIN");

  drawTrendPanel(col0X, row3Y, panelW, panelH, "AVIATION AGE", trendGetAviationAge, screen.color565(60, 170, 220), "MIN");
  drawTrendPanel(col1X, row3Y, panelW, panelH, "ISS AGE", trendGetIssAge, screen.color565(150, 170, 235), "MIN");
  drawTrendPanel(col2X, row3Y, panelW, panelH, "SPACEX AGE", trendGetSpacexAge, screen.color565(220, 150, 60), "MIN");
  drawTrendPanel(col3X, row3Y, panelW, panelH, "SPX DTL AGE", trendGetSpacexDetailAge, screen.color565(200, 100, 100), "MIN");
}

static void draw_placeholder(const char* label) {
  StateLockGuard lockGuard;
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
  colorStarshipDay = screen.color565(255, 165, 0); // orange/gold -- Starship/Super Heavy highlight

  // Night mode: shades of red only, to preserve night vision for
  // astrophotography. Meaning that used to come from hue (blue vs green
  // vs orange) now comes from brightness instead. Starship/Super Heavy's
  // highlight keeps that rule (no true orange at night) -- it's a
  // distinctly lighter/pinker red instead of a new hue, still readable
  // as its own color against Accent/Success/Danger's darker reds.
  colorBgNight = screen.color565(10, 0, 0);
  colorTextNight = screen.color565(210, 40, 40);
  colorDimNight = screen.color565(90, 15, 15);
  colorAccentNight = screen.color565(160, 30, 30);
  colorSuccessNight = screen.color565(180, 50, 50);
  colorDangerNight = screen.color565(255, 60, 60);
  colorStarshipNight = screen.color565(255, 130, 130);

  colorBg = colorBgDay;
  colorText = colorTextDay;
  colorDim = colorDimDay;
  colorAccent = colorAccentDay;
  colorSuccess = colorSuccessDay;
  colorDanger = colorDangerDay;
  colorStarship = colorStarshipDay;

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
  StateLockGuard lockGuard;
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

  // Good ISS pass coming up in ~30 minutes -- after sunset, clear skies
  // (no cloud cover, no precip), and a high (>45deg) elevation pass, all
  // checked against the astro forecast point closest to the pass's start
  // time rather than right-now conditions, since the pass itself is still
  // ~30 minutes out.
  bool issGoodPassSoon = false;
  if (g_issPassCount > 0 && g_weather.valid && g_astroForecastCount > 0) {
    IssPass& nextIssPass = g_issPasses[0];
    if (nextIssPass.maxElevationDeg > 45) {
      uint32_t nowUnix = (uint32_t)time(nullptr);
      if (nowUnix > 100000) {
        uint32_t alertWindowStart = (nextIssPass.startUnix > 1800) ? (nextIssPass.startUnix - 1800) : 0;
        bool inAlertWindow = (nowUnix >= alertWindowStart && nowUnix < nextIssPass.startUnix);
        bool afterSunset = nowUnix >= g_weather.sunsetUnix;
        if (inAlertWindow && afterSunset) {
          // Closest forecast point (3-hour spacing) to the pass start time.
          int forecastIdx = -1;
          uint32_t bestDiff = 0xFFFFFFFF;
          for (int i = 0; i < g_astroForecastCount; i++) {
            uint32_t diff = (g_astroForecast[i].unixTime > nextIssPass.startUnix)
                                ? (g_astroForecast[i].unixTime - nextIssPass.startUnix)
                                : (nextIssPass.startUnix - g_astroForecast[i].unixTime);
            if (diff < bestDiff) { bestDiff = diff; forecastIdx = i; }
          }
          if (forecastIdx >= 0) {
            // cloudcover is a 1-9 index (1=clearest) -- treating <=2 as
            // "no cloud coverage" rather than requiring the exact
            // clearest bucket, which is rarely hit precisely.
            bool clearWeather = g_astroForecast[forecastIdx].prectype.equalsIgnoreCase("none");
            bool noCloudCover = g_astroForecast[forecastIdx].cloudcover <= 2;
            if (clearWeather && noCloudCover) {
              issGoodPassSoon = true;
            }
          }
        }
      }
    }
  }

  // Starship and Super Heavy launching today -- two distinct alerts, not
  // one combined "Starship/Super Heavy" message, compared by local
  // calendar date so each fires once as soon as it becomes "today".
  // Super Heavy takes priority if a rocket name somehow contains both
  // terms, so only one of the two ever fires for the same launch.
  bool starshipLaunchToday = false;
  bool superHeavyLaunchToday = false;
  if (g_spacexValid && g_spacexLaunchCount > 0) {
    String lowerRocketName = g_spacexLaunches[0].rocketName;
    lowerRocketName.toLowerCase();
    bool isSuperHeavy = lowerRocketName.indexOf("super heavy") >= 0;
    bool isStarshipVehicle = lowerRocketName.indexOf("starship") >= 0;
    time_t nowUnix = time(nullptr);
    if (nowUnix > 100000 && (isSuperHeavy || isStarshipVehicle)) {
      time_t launchUnix = (time_t)g_spacexLaunches[0].netUnix;
      // Copy each result out immediately -- localtime() returns a pointer
      // to a shared static buffer that the second call would overwrite.
      struct tm nowTm = *localtime(&nowUnix);
      struct tm launchTm = *localtime(&launchUnix);
      bool isToday = (nowTm.tm_year == launchTm.tm_year && nowTm.tm_yday == launchTm.tm_yday);
      if (isToday) {
        if (isSuperHeavy) {
          superHeavyLaunchToday = true;
        } else {
          starshipLaunchToday = true;
        }
      }
    }
  }

  // Separate 30-minutes-before alert for Starship/Super Heavy, alongside
  // the "launching today" alert above -- same Super-Heavy-priority rule
  // if a name somehow contains both terms.
  bool starshipLaunch30Min = false;
  bool superHeavyLaunch30Min = false;
  if (g_spacexValid && g_spacexLaunchCount > 0) {
    String lowerRocketName30 = g_spacexLaunches[0].rocketName;
    lowerRocketName30.toLowerCase();
    bool isSuperHeavy30 = lowerRocketName30.indexOf("super heavy") >= 0;
    bool isStarshipVehicle30 = lowerRocketName30.indexOf("starship") >= 0;
    if (isSuperHeavy30 || isStarshipVehicle30) {
      uint32_t nowUnix30 = (uint32_t)time(nullptr);
      if (nowUnix30 > 100000) {
        uint32_t launchUnix30 = g_spacexLaunches[0].netUnix;
        uint32_t windowStart30 = (launchUnix30 > 1800) ? (launchUnix30 - 1800) : 0;
        bool inWindow30 = (nowUnix30 >= windowStart30 && nowUnix30 < launchUnix30);
        if (inWindow30) {
          if (isSuperHeavy30) {
            superHeavyLaunch30Min = true;
          } else {
            starshipLaunch30Min = true;
          }
        }
      }
    }
  }

  if (g_alertStatePrimed) {
    // Independent ifs, not else-if -- every condition that newly became
    // true this frame gets queued, so simultaneous events (e.g. a storm
    // warning and a Starship launch-day alert on the same frame) both
    // get shown in turn instead of one silently overwriting the other.
    if (anyEmergencyNow && !g_prevAnyEmergency) {
      enqueueAlert("EMERGENCY SQUAWK DETECTED NEARBY", colorDanger);
    }
    if (stormIsHigh && !g_prevStormWasHigh) {
      enqueueAlert("ASTRO STORM RISK: HIGH RISK TONIGHT", colorDanger);
    }
    if (astroIsGood && !g_prevAstroWasGood) {
      enqueueAlert("ASTRO CONDITIONS NOW GOOD TONIGHT", colorSuccess);
    }
    if (starshipLaunchToday && !g_prevStarshipLaunchToday) {
      enqueueAlert("STARSHIP LAUNCH TODAY", colorStarship);
    }
    if (superHeavyLaunchToday && !g_prevSuperHeavyLaunchToday) {
      enqueueAlert("SUPER HEAVY LAUNCH TODAY", colorStarship);
    }
    if (issGoodPassSoon && !g_prevIssGoodPassSoon) {
      enqueueAlert("GOOD ISS PASS IN 30 MIN", colorSuccess);
    }
    if (starshipLaunch30Min && !g_prevStarshipLaunch30Min) {
      enqueueAlert("STARSHIP LAUNCH IN 30 MIN", colorStarship);
    }
    if (superHeavyLaunch30Min && !g_prevSuperHeavyLaunch30Min) {
      enqueueAlert("SUPER HEAVY LAUNCH IN 30 MIN", colorStarship);
    }
    if (!astroIsGood && g_prevAstroWasGood && g_alertActive &&
        strcmp(g_alertMessage, "ASTRO CONDITIONS NOW GOOD TONIGHT") == 0) {
      // The banner only ever fired once on the moment of transition and
      // then sat frozen until tapped -- if a later Astro data refresh
      // brings the verdict back down (e.g. seeing worsens even though
      // cloud cover/transparency stayed good), the banner was left
      // showing stale "GOOD" text that no longer matched the Astro
      // page's own live verdict. Auto-clear specifically when the
      // condition it announced has reverted, rather than on any fixed
      // timer -- this is not the auto-timeout that was removed earlier,
      // it only reacts to the underlying data actually changing. Now
      // advances to the next queued alert (if any) instead of just
      // going blank.
      advanceAlertQueue();
    }
  }

  g_prevAstroWasGood = astroIsGood;
  g_prevStormWasHigh = stormIsHigh;
  g_prevAnyEmergency = anyEmergencyNow;
  g_prevStarshipLaunchToday = starshipLaunchToday;
  g_prevSuperHeavyLaunchToday = superHeavyLaunchToday;
  g_prevIssGoodPassSoon = issGoodPassSoon;
  g_prevStarshipLaunch30Min = starshipLaunch30Min;
  g_prevSuperHeavyLaunch30Min = superHeavyLaunch30Min;
  g_alertStatePrimed = true;

}

// Drawn last, on top of everything else (including the header), so it's
// a true takeover regardless of which page is showing or whether the
// page is locked.
static void drawAlertBanner() {
  StateLockGuard lockGuard;
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

// Alert queue: if two+ conditions become newly-true in the same frame,
// only the first one used to ever get shown -- the others' "previous
// state" flags flipped to true anyway, so their transition was silently
// lost and would never be shown, even later. Queuing instead of
// dropping means nothing gets missed; each queued alert gets its turn
// once the current one is dismissed (by tap, or an auto-clear like the
// astro-good reversal).
struct PendingAlert {
  char message[64];
  uint16_t color;
};
static const int ALERT_QUEUE_MAX = 6;
static PendingAlert g_alertQueue[ALERT_QUEUE_MAX];
static int g_alertQueueCount = 0;

static void enqueueAlert(const char* message, uint16_t color) {
  if (!g_alertActive) {
    g_alertActive = true;
    g_alertShownAtMs = millis();
    g_alertColorOverride = color;
    snprintf(g_alertMessage, sizeof(g_alertMessage), "%s", message);
    return;
  }
  if (g_alertQueueCount < ALERT_QUEUE_MAX) {
    snprintf(g_alertQueue[g_alertQueueCount].message, sizeof(g_alertQueue[g_alertQueueCount].message), "%s", message);
    g_alertQueue[g_alertQueueCount].color = color;
    g_alertQueueCount++;
  }
}

// Advances to the next queued alert (if any) instead of just clearing --
// called wherever the banner used to simply go blank.
static void advanceAlertQueue() {
  if (g_alertQueueCount > 0) {
    g_alertActive = true;
    g_alertShownAtMs = millis();
    g_alertColorOverride = g_alertQueue[0].color;
    snprintf(g_alertMessage, sizeof(g_alertMessage), "%s", g_alertQueue[0].message);
    for (int i = 1; i < g_alertQueueCount; i++) {
      g_alertQueue[i - 1] = g_alertQueue[i];
    }
    g_alertQueueCount--;
  } else {
    g_alertActive = false;
  }
}

// True if this rocket is Starship or Super Heavy -- SpaceX's next-gen
// vehicle, worth calling out distinctly from routine Falcon 9 flights.
static bool isStarshipOrSuperHeavy(const String& rocketName) {
  String lower = rocketName;
  lower.toLowerCase();
  return lower.indexOf("starship") >= 0 || lower.indexOf("super heavy") >= 0;
}

// Falcon 9 / Falcon Heavy -- SpaceX's workhorse family, the majority of
// launches on this page day-to-day. Gets its own badge treatment (blue,
// falcon/hawk icon) distinct from the rarer Starship/Super Heavy gold
// badge, instead of being lumped into a generic fallback.
static bool isFalconClass(const String& rocketName) {
  String lower = rocketName;
  lower.toLowerCase();
  return lower.indexOf("falcon") >= 0;
}

// Compact rocket silhouette (nose cone + body + fins), built from the
// same fillTriangle/fillRect primitives used throughout this project --
// no custom bitmap needed. (cx, cy) anchors the top-left of a ~20x32
// bounding box.
static void drawRocketIcon(int cx, int cy, uint16_t color) {
  int w = 20, h = 32;
  int bodyW = 10;
  int bodyX = cx + (w - bodyW) / 2;

  // Nose cone (top third).
  screen.fillTriangle(cx + w / 2, cy, bodyX, cy + h / 3, bodyX + bodyW, cy + h / 3, color);
  // Body (middle half).
  screen.fillRect(bodyX, cy + h / 3, bodyW, h / 2, color);
  // Fins (bottom corners).
  int finY = cy + h / 3 + h / 2;
  screen.fillTriangle(bodyX, finY, bodyX - 6, finY + 8, bodyX, finY + 8, color);
  screen.fillTriangle(bodyX + bodyW, finY, bodyX + bodyW + 6, finY + 8, bodyX + bodyW, finY + 8, color);
}

// Hawk-in-flight silhouette (pointed head/beak, tapering body, swept-back
// wings) for the Falcon-family badge -- same ~20x32 bounding box and
// fillTriangle-only construction style as drawRocketIcon(), so the two
// badges sit at consistent size/position regardless of which one shows.
static void drawFalconIcon(int cx, int cy, uint16_t color) {
  int w = 20, h = 32;
  int centerX = cx + w / 2;

  // Head/beak (small triangle at the very top).
  screen.fillTriangle(centerX, cy, centerX - 3, cy + 6, centerX + 3, cy + 6, color);
  // Body (tapers from the head down to a point at the tail).
  screen.fillTriangle(centerX - 3, cy + 6, centerX + 3, cy + 6, centerX, cy + h, color);
  // Wings -- swept back from mid-body, classic hawk-from-below silhouette.
  int wingY = cy + 10;
  screen.fillTriangle(centerX, wingY, cx, wingY + 4, centerX - 2, wingY + 14, color);
  screen.fillTriangle(centerX, wingY, cx + w, wingY + 4, centerX + 2, wingY + 14, color);
}

static void draw_spacex() {
  StateLockGuard lockGuard;
  screen.setTextDatum(textdatum_t::top_left);

  if (!g_spacexValid) {
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("No launch data yet", 20, 100);
    char errLine[48];
    snprintf(errLine, sizeof(errLine), "Last HTTP result: %d", g_spacexLastHttpCode);
    screen.drawString(errLine, 20, 140);
    return;
  }

  if (g_spacexLaunchCount == 0) {
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("No SpaceX launches in the next 30 days", 20, 100);
    return;
  }

  // ---------- Featured block: the next launch, with image + landing info ----------
  SpacexLaunch& next = g_spacexLaunches[0];
  int y = 50;

  if (g_spacexImageValid && g_spacexImagePixels != nullptr) {
    // Now pre-downsampled in spacex_launch_service.cpp to a fixed 100px
    // (1.25in) tall thumbnail, width aspect-preserved. This is the
    // shared "row height" the Starship/Super Heavy badge below is
    // vertically centered against. Moved left 5px and up 4px (was
    // x=620, y=64) per follow-up feedback -- bottom edge now lands at
    // y=160, still 6px clear of the divider line at y=166.
    screen.drawRGBBitmap(615, 60, g_spacexImagePixels, g_spacexImageWidth, g_spacexImageHeight);
  }

  time_t t = (time_t)next.netUnix;
  struct tm* ti = localtime(&t);
  char dateBuf[8], timeBuf[8];
  strftime(dateBuf, sizeof(dateBuf), "%b%d", ti);
  int h12 = ti->tm_hour % 12;
  if (h12 == 0) h12 = 12;
  snprintf(timeBuf, sizeof(timeBuf), "%d:%02d%s", h12, ti->tm_min, ti->tm_hour < 12 ? "A" : "P");

  uint16_t statusColor = colorText;
  if (next.statusName.equalsIgnoreCase("Go") || next.statusName.equalsIgnoreCase("Success")) {
    statusColor = colorSuccess;
  } else if (next.statusName.equalsIgnoreCase("TBD") || next.statusName.equalsIgnoreCase("Hold")) {
    statusColor = colorDim;
  } else if (next.statusName.equalsIgnoreCase("Failure")) {
    statusColor = colorDanger;
  }

  // EST/EDT determined dynamically from tm_isdst (set correctly by
  // configTzTime's "EST5EDT,M3.2.0,M11.1.0" rule in main.cpp) rather
  // than hardcoded, so it's right whether DST is in effect or not.
  const char* tzLabel = (ti->tm_isdst > 0) ? "EDT" : "EST";

  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString("NEXT LAUNCH", 20, y);
  y += 26;

  char line1[28];
  snprintf(line1, sizeof(line1), "%s  %s %s", dateBuf, timeBuf, tzLabel);
  screen.drawString(line1, 20, y);
  screen.setTextColor(statusColor, colorBg);
  screen.drawString(next.statusName.c_str(), 260, y - 10); // nudged up 1/8in (10px) to clear the badge icon below it
  y += 28;

  // Every launch now gets the icon+colored-name badge treatment (was
  // Starship/Super Heavy only, with everything else falling back to
  // plain "RocketName - MissionName" text): gold rocket icon for
  // Starship/Super Heavy, blue falcon/hawk icon for the Falcon family,
  // dim rocket icon for anything else. Badge position/sizing (icon at
  // 350,94, text at 378,99) and the mission-name line below it are the
  // same in all three cases, only the icon/color/label text differ.
  bool nextIsStarship = isStarshipOrSuperHeavy(next.rocketName);
  bool nextIsFalcon = isFalconClass(next.rocketName);
  {
    String badgeLabelStr = next.rocketName;
    uint16_t badgeColor;
    if (nextIsStarship) {
      String lowerRocket = next.rocketName;
      lowerRocket.toLowerCase();
      bool nextIsSuperHeavy = lowerRocket.indexOf("super heavy") >= 0;
      badgeLabelStr = nextIsSuperHeavy ? "SUPER HEAVY" : "STARSHIP";
      badgeColor = colorStarship;
      drawRocketIcon(350, 94, badgeColor);
    } else if (nextIsFalcon) {
      badgeLabelStr.toUpperCase();
      badgeColor = colorAccent;
      drawFalconIcon(350, 94, badgeColor);
    } else {
      badgeLabelStr.toUpperCase();
      badgeColor = colorDim;
      drawRocketIcon(350, 94, badgeColor);
    }

    // Vertically centered against the image's 100px-tall row (image
    // top y=60, row center y=60+50=110): icon (32px tall) top = 110-16
    // =94, text (FONT_H=7 at size 3 = 21px tall) top = 110-11=99 --
    // both landing on the same mid-row line as the image.
    screen.setTextSize(3);
    screen.setTextColor(badgeColor, colorBg);
    screen.drawString(badgeLabelStr.c_str(), 378, 99);

    screen.setTextSize(1);
    screen.setTextColor(colorText, colorBg);
    screen.drawString(next.missionName.c_str(), 20, y);
    y += 18;
  }

  screen.setTextColor(colorDim, colorBg);
  char line3[96];
  snprintf(line3, sizeof(line3), "%s, %s", next.padName.c_str(), next.locationName.c_str());
  screen.drawString(line3, 20, y);
  y += 18;

  // Booster landing info, if known -- absent for expendable missions or
  // before the separate detail fetch has completed.
  screen.setTextColor(colorDim, colorBg);
  char landingLine[96];
  if (!g_spacexLandingValid) {
    snprintf(landingLine, sizeof(landingLine), "Landing info: pending");
  } else if (!g_spacexLandingAttempt) {
    snprintf(landingLine, sizeof(landingLine), "Landing: no attempt planned");
  } else {
    snprintf(landingLine, sizeof(landingLine), "Landing: %s (%s) - %s",
             g_spacexLandingLocation.c_str(), g_spacexLandingAbbrev.c_str(), g_spacexLandingType.c_str());
  }
  screen.drawString(landingLine, 20, y);
  y += 26;

  screen.drawLine(20, y, WIDTH - 20, y, colorDim);
  y += 12;

  // ---------- Remaining upcoming launches, compact list ----------
  int shown = min(g_spacexLaunchCount, 5); // index 0 already featured above
  for (int i = 1; i < shown; i++) {
    SpacexLaunch& launch = g_spacexLaunches[i];

    time_t lt = (time_t)launch.netUnix;
    struct tm* lti = localtime(&lt);
    char lDateBuf[8], lTimeBuf[8];
    strftime(lDateBuf, sizeof(lDateBuf), "%b%d", lti);
    int lh12 = lti->tm_hour % 12;
    if (lh12 == 0) lh12 = 12;
    snprintf(lTimeBuf, sizeof(lTimeBuf), "%d:%02d%s", lh12, lti->tm_min, lti->tm_hour < 12 ? "A" : "P");

    uint16_t lStatusColor = colorText;
    if (launch.statusName.equalsIgnoreCase("Go") || launch.statusName.equalsIgnoreCase("Success")) {
      lStatusColor = colorSuccess;
    } else if (launch.statusName.equalsIgnoreCase("TBD") || launch.statusName.equalsIgnoreCase("Hold")) {
      lStatusColor = colorDim;
    } else if (launch.statusName.equalsIgnoreCase("Failure")) {
      lStatusColor = colorDanger;
    }

    const char* lTzLabel = (lti->tm_isdst > 0) ? "EDT" : "EST";

    screen.setTextSize(2);
    screen.setTextColor(colorAccent, colorBg);
    char lLine1[28];
    snprintf(lLine1, sizeof(lLine1), "%s  %s %s", lDateBuf, lTimeBuf, lTzLabel);
    screen.drawString(lLine1, 20, y);

    screen.setTextColor(lStatusColor, colorBg);
    screen.drawString(launch.statusName.c_str(), 260, y);

    screen.setTextSize(1);
    screen.setTextColor(colorText, colorBg);
    char lLine2[80];
    snprintf(lLine2, sizeof(lLine2), "%s - %s", launch.rocketName.c_str(), launch.missionName.c_str());
    screen.drawString(lLine2, 20, y + 24);

    screen.setTextColor(colorDim, colorBg);
    char lLine3[96];
    snprintf(lLine3, sizeof(lLine3), "%s, %s", launch.padName.c_str(), launch.locationName.c_str());
    screen.drawString(lLine3, 20, y + 40);

    // Starship/Super Heavy badge to the right of the row -- plenty of
    // open space there, rather than folding the rocket name into the
    // same left-hand text column as every other entry.
    if (isStarshipOrSuperHeavy(launch.rocketName)) {
      String lowerRocket = launch.rocketName;
      lowerRocket.toLowerCase();
      bool rowIsSuperHeavy = lowerRocket.indexOf("super heavy") >= 0;
      const char* badgeLabel = rowIsSuperHeavy ? "SUPER HEAVY" : "STARSHIP";
      drawRocketIcon(560, y, colorStarship);
      screen.setTextSize(2);
      screen.setTextColor(colorStarship, colorBg);
      screen.drawString(badgeLabel, 588, y + 8);
    }

    y += 62;
    if (i < shown - 1) {
      screen.drawLine(20, y - 8, WIDTH - 20, y - 8, colorDim);
    }
  }

  // Legend for both badge icons used above -- bottom-right corner, out
  // of the way of the launch list itself. Falcon entry stacked above the
  // Starship/Super Heavy one, same x position.
  {
    int legendIconX = WIDTH - 180; // moved left ~1/4in -- was clipping off the right edge
    int legendIconY = HEIGHT - 40;
    drawRocketIcon(legendIconX, legendIconY, colorStarship);
    screen.setTextSize(1);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("= Starship/Super Heavy", legendIconX + 26, legendIconY + 10);

    int falconLegendY = legendIconY - 44; // nudged up 1/8in (10px) further to clear icon overlap, 20px total from icon row
    drawFalconIcon(legendIconX, falconLegendY, colorAccent);
    screen.drawString("= Falcon 9/Heavy", legendIconX + 26, falconLegendY + 10);
  }
}

// Finds the best (lowest-badness) forecast point within a given LOCAL
// calendar day (0=today, 1=tomorrow, 2=day after), restricted to
// nighttime hours -- same isNight convention used elsewhere in this file
// (tm_hour >= 20 || tm_hour < 6). 7Timer's timepoints are UTC-based, so
// this converts each point to local time via localtime() before checking
// which calendar day and hour it falls in, rather than doing raw unix-
// time arithmetic that would misplace points near local midnight.
static int findBestWvIndexForLocalDay(int dayOffset, float* outBadness) {
  time_t nowUnix = time(nullptr);
  struct tm nowTm = *localtime(&nowUnix);
  int todayYday = nowTm.tm_yday;
  int todayYear = nowTm.tm_year;

  int bestIdx = -1;
  float bestBadness = 2.0f;
  for (int i = 0; i < g_wvAstroForecastCount; i++) {
    time_t t = (time_t)g_wvAstroForecast[i].unixTime;
    struct tm ti = *localtime(&t);
    bool isNight = (ti.tm_hour >= 20 || ti.tm_hour < 6);
    if (!isNight) continue;
    int ydayDelta = ti.tm_yday - todayYday;
    if (ti.tm_year != todayYear) ydayDelta += (ti.tm_year > todayYear) ? 365 : -365;
    if (ydayDelta != dayOffset) continue;

    float badness = 0;
    astro_tonight_verdict(g_wvAstroForecast[i].cloudcover, g_wvAstroForecast[i].seeing,
                           g_wvAstroForecast[i].transparency, g_moonIllumPercent, &badness);
    if (badness < bestBadness) {
      bestBadness = badness;
      bestIdx = i;
    }
  }
  if (outBadness) *outBadness = bestBadness;
  return bestIdx;
}

// Maps a condition label ("GOOD"/"FAIR"/"POOR"/"BAD"/"--") to the same
// 4-color scheme used for verdict/cloud coloring elsewhere on this page --
// green/yellow/orange/red, dim for the no-data fallback.
static uint16_t conditionLabelColor(const char* label) {
  if (strcmp(label, "GOOD") == 0) return colorSuccess;
  if (strcmp(label, "FAIR") == 0) return screen.color565(230, 200, 40);
  if (strcmp(label, "POOR") == 0) return screen.color565(230, 130, 40);
  if (strcmp(label, "BAD") == 0) return colorDanger;
  return colorDim;
}

// 5-day astrophotography trip-planning forecast for Spruce Knob, WV
// (Bortle 2 vs Bortle 7.4 at home), plus the aurora/Kp indicator -- same
// Kp value applies to both locations, shown once rather than duplicated.
// Days 1-3 use real 7Timer seeing/transparency data via the same
// astro_tonight_verdict() scoring as the home Astro page. Days 4-5 are
// Open-Meteo cloud-cover-only (no free source has real seeing data this
// far out) and are deliberately styled dimmer/simpler to signal lower
// confidence -- see wv_astro_service.h for the reasoning.
static void draw_wv_astro() {
  StateLockGuard lockGuard;
  screen.setTextDatum(textdatum_t::top_left);

  screen.setTextSize(2);
  screen.setTextColor(screen.color565(255, 255, 255), colorBg); // fixed white, per follow-up feedback
  screen.drawString("SPRUCE KNOB, WV - 5 DAY FORECAST", 20, 50);

  // Bortle scale comparison, moved to the right edge (matching the home
  // Astro page's Bortle-indicator placement) to free up left-side space
  // for the Best Night callout below. Two markers instead of one, so the
  // whole point of this page (WV is MUCH darker than home) is visible at
  // a glance instead of requiring the viewer to already know what a
  // 5-point Bortle gap actually looks like.
  {
    int barX = 520, barY = 76, barW = 260, barH = 8;

    static const uint8_t bortleStops[5][3] = {
      {80, 200, 120}, {160, 200, 60}, {230, 200, 40}, {230, 130, 40}, {220, 60, 60}
    };
    for (int px = 0; px < barW; px += 2) {
      float frac = (float)px / (float)(barW - 1);
      screen.fillRect(barX + px, barY, 2, barH, multiStopGradient(frac, bortleStops, 5));
    }
    float wvFrac = constrain((WV_BORTLE_CLASS - 1.0f) / 8.0f, 0.0f, 1.0f);
    int wvPointerX = barX + (int)(wvFrac * (barW - 1));
    screen.fillTriangle(wvPointerX - 4, barY - 5, wvPointerX + 4, barY - 5, wvPointerX, barY - 1, colorSuccess);

    float homeFrac = constrain((HOME_BORTLE_CLASS - 1.0f) / 8.0f, 0.0f, 1.0f);
    int homePointerX = barX + (int)(homeFrac * (barW - 1));
    uint16_t homeMarkerColor = screen.color565(230, 130, 40); // orange, matching this project's established POOR/warning color
    screen.fillTriangle(homePointerX - 4, barY + barH + 5, homePointerX + 4, barY + barH + 5, homePointerX, barY + barH + 1, homeMarkerColor);

    // Labels centered over their own markers -- "WV 2.0" over the green
    // triangle, "Home 7.4" over the dim triangle, "vs" centered in the
    // gap between them -- rather than one flat left-aligned line, now
    // that marker positions are known before the labels are drawn.
    screen.setTextSize(1);
    int labelY = 55;

    char wvLabel[16];
    snprintf(wvLabel, sizeof(wvLabel), "WV %.1f", (double)WV_BORTLE_CLASS);
    int wvLabelW = screen.textWidth(wvLabel);
    screen.setTextColor(colorSuccess, colorBg);
    screen.drawString(wvLabel, wvPointerX - wvLabelW / 2, labelY);

    char homeLabel[16];
    snprintf(homeLabel, sizeof(homeLabel), "Home %.1f", (double)HOME_BORTLE_CLASS);
    int homeLabelW = screen.textWidth(homeLabel);
    screen.setTextColor(homeMarkerColor, colorBg);
    screen.drawString(homeLabel, homePointerX - homeLabelW / 2, labelY);

    int vsLabelW = screen.textWidth("vs");
    int vsCenterX = (wvPointerX + homePointerX) / 2;
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("vs", vsCenterX - vsLabelW / 2, labelY);
  }

  // Best Night across the 3 real-data days -- days 4-5 have no seeing/
  // transparency score to compare against, so they're deliberately
  // excluded from this callout. Same colored-badness pattern as the home
  // Astro page's "Best: <date> <time>".
  {
    int bestOverallIdx = -1;
    float bestOverallBadness = 2.0f;
    for (int d = 0; d < 3; d++) {
      float badness = 1.0f;
      int idx = findBestWvIndexForLocalDay(d, &badness);
      if (idx >= 0 && badness < bestOverallBadness) {
        bestOverallBadness = badness;
        bestOverallIdx = idx;
      }
    }
    if (bestOverallIdx >= 0) {
      uint16_t bestColor;
      // Fixed to always-green per follow-up feedback (was previously
      // dynamic, scaling with how good/bad the best night's conditions
      // were).
      bestColor = colorSuccess;

      char bestDateBuf[16];
      char bestTimeBuf[16];
      formatPassDate(g_wvAstroForecast[bestOverallIdx].unixTime, bestDateBuf, sizeof(bestDateBuf));
      formatPassTime(g_wvAstroForecast[bestOverallIdx].unixTime, bestTimeBuf, sizeof(bestTimeBuf));
      char bestLine[40];
      snprintf(bestLine, sizeof(bestLine), "Best: %s %s", bestDateBuf, bestTimeBuf);
      screen.setTextSize(2);
      screen.setTextColor(bestColor, colorBg);
      screen.drawString(bestLine, 20, 80);
    }
  }

  // Aurora/Kp -- single planetary value, identical for WV and home, so
  // shown once here rather than duplicated per forecast day. Rating word
  // color-coded on a Likely=green -> Unlikely=red scale, same GOOD->BAD
  // ramp already used for the S:/T:/C: condition labels elsewhere.
  {
    char auroraPrefix[32];
    const char* auroraLabel = nullptr;
    uint16_t auroraLabelColor = colorText;
    if (g_kpObservedValid) {
      snprintf(auroraPrefix, sizeof(auroraPrefix), "AURORA (Kp %.1f): ", (double)g_currentKp);
      auroraLabel = aurora_visibility_label(g_currentKp);
      if (strcmp(auroraLabel, "Likely") == 0) auroraLabelColor = colorSuccess;
      else if (strcmp(auroraLabel, "Possible") == 0) auroraLabelColor = screen.color565(230, 200, 40);
      else if (strcmp(auroraLabel, "Slight Chance") == 0) auroraLabelColor = screen.color565(230, 130, 40);
      else auroraLabelColor = colorDanger;
    } else {
      snprintf(auroraPrefix, sizeof(auroraPrefix), "AURORA: no data yet");
    }
    screen.setTextSize(2);
    screen.setTextColor(colorAccent, colorBg);
    screen.drawString(auroraPrefix, 20, 120);
    if (auroraLabel != nullptr) {
      int labelX = 20 + screen.textWidth(auroraPrefix);
      screen.setTextColor(auroraLabelColor, colorBg);
      screen.drawString(auroraLabel, labelX, 120);
    }
  }

  if (g_wvAstroForecastCount == 0) {
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("No WV astro data yet", 20, 165);
    char httpLine[48];
    snprintf(httpLine, sizeof(httpLine), "Last HTTP result: %d", g_wvAstroLastHttpCode);
    screen.drawString(httpLine, 20, 193);
    screen.setTextSize(1);
    screen.drawString(g_wvAstroLastFailureReason, 20, 221);
    return;
  }

  int colW = (WIDTH - 40) / 5;
  int colStartX = 20;
  int colY = 165;
  const char* dayLabels[5] = {"TONIGHT", "TOMORROW", "DAY 3", "DAY 4", "DAY 5"};

  // Days 1-3: real 7Timer seeing/transparency data, same verdict scoring
  // as the home Astro page, now with the calendar date, storm risk, and
  // precip type also shown -- all three were being fetched already but
  // not displayed anywhere on this page.
  for (int d = 0; d < 3; d++) {
    float badness = 1.0f;
    int idx = findBestWvIndexForLocalDay(d, &badness);
    int x = colStartX + d * colW;

    screen.setTextSize(2);
    screen.setTextColor(colorText, colorBg);
    screen.drawString(dayLabels[d], x, colY);

    if (idx < 0) {
      screen.setTextSize(1);
      screen.setTextColor(colorDim, colorBg);
      screen.drawString("no data", x, colY + 30);
      continue;
    }

    char dateBuf[16];
    formatPassDate(g_wvAstroForecast[idx].unixTime, dateBuf, sizeof(dateBuf));
    screen.setTextSize(1);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString(dateBuf, x, colY + 18);

    const char* verdict = astro_tonight_verdict(
        g_wvAstroForecast[idx].cloudcover, g_wvAstroForecast[idx].seeing,
        g_wvAstroForecast[idx].transparency, g_moonIllumPercent, &badness);

    uint16_t verdictColor;
    if (badness < 0.25f) verdictColor = colorSuccess;
    else if (badness < 0.5f) verdictColor = screen.color565(230, 200, 40);
    else if (badness < 0.75f) verdictColor = screen.color565(230, 130, 40);
    else verdictColor = colorDanger;

    screen.setTextSize(2);
    screen.setTextColor(verdictColor, colorBg);
    screen.drawString(verdict, x, colY + 38);

    screen.setTextSize(1);
    {
      // Each condition word gets its own color (GOOD=green, FAIR=yellow,
      // POOR=orange, BAD=red) instead of one flat dim line -- the "S:"/
      // "T:"/"C:" prefixes (including the colon) stay white throughout.
      // screen.textWidth() gives the exact pixel width per segment (per
      // this file's own convention for precise text positioning) so
      // segments sit flush against each other with no guessed spacing.
      const char* seeingLabel = astro_seeing_label(g_wvAstroForecast[idx].seeing);
      const char* transparencyLabel = astro_transparency_label(g_wvAstroForecast[idx].transparency);
      const char* cloudLabel = astro_cloudcover_label(g_wvAstroForecast[idx].cloudcover);
      int lineX = x;
      int lineY = colY + 68;

      screen.setTextColor(colorText, colorBg);
      screen.drawString("S:", lineX, lineY);
      lineX += screen.textWidth("S:");
      screen.setTextColor(conditionLabelColor(seeingLabel), colorBg);
      screen.drawString(seeingLabel, lineX, lineY);
      lineX += screen.textWidth(seeingLabel);

      screen.setTextColor(colorText, colorBg);
      screen.drawString(" T:", lineX, lineY);
      lineX += screen.textWidth(" T:");
      screen.setTextColor(conditionLabelColor(transparencyLabel), colorBg);
      screen.drawString(transparencyLabel, lineX, lineY);
      lineX += screen.textWidth(transparencyLabel);

      screen.setTextColor(colorText, colorBg);
      screen.drawString(" C:", lineX, lineY);
      lineX += screen.textWidth(" C:");
      screen.setTextColor(conditionLabelColor(cloudLabel), colorBg);
      screen.drawString(cloudLabel, lineX, lineY);
    }

    // Storm risk -- same 4-tier thresholds and astro_instability_label()
    // as the home Astro page's STORM RISK section (just without its
    // legend line -- no room per-column in this 5-column layout), now
    // with a white "STORM: " prefix, same segmented-coloring technique
    // as the S:/T:/C: line above.
    {
      int li = g_wvAstroForecast[idx].liftedindex;
      uint16_t stormColor;
      if (li > 0) stormColor = colorSuccess;
      else if (li > -4) stormColor = screen.color565(230, 200, 40);
      else if (li > -8) stormColor = screen.color565(230, 130, 40);
      else stormColor = colorDanger;

      int stormX = x;
      screen.setTextColor(colorText, colorBg);
      screen.drawString("STORM: ", stormX, colY + 88);
      stormX += screen.textWidth("STORM: ");
      screen.setTextColor(stormColor, colorBg);
      screen.drawString(astro_instability_label(li), stormX, colY + 88);
    }

    // Precip -- only drawn when there's actually something to warn about,
    // same "prectype != none" gate as the home Astro page.
    if (g_wvAstroForecast[idx].prectype != "none") {
      char precipLine[24];
      snprintf(precipLine, sizeof(precipLine), "Precip: %s", g_wvAstroForecast[idx].prectype.c_str());
      screen.setTextColor(colorDim, colorBg);
      screen.drawString(precipLine, x, colY + 104);
    }
  }

  // Days 4-5: cloud-only, Open-Meteo -- deliberately dimmer styling and no
  // verdict word, since this is lower-confidence data than days 1-3.
  for (int n = 0; n < WV_CLOUD_ONLY_NIGHTS; n++) {
    int x = colStartX + (3 + n) * colW;

    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString(dayLabels[3 + n], x, colY);

    if (g_wvCloudOnlyValid && g_wvCloudOnlyNights[n].nightDateUnix > 0) {
      char dateBuf[16];
      formatPassDate(g_wvCloudOnlyNights[n].nightDateUnix, dateBuf, sizeof(dateBuf));
      screen.setTextSize(1);
      screen.setTextColor(colorDim, colorBg);
      screen.drawString(dateBuf, x, colY + 18);
    }

    screen.setTextSize(1);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("(cloud only)", x, colY + 34);

    if (!g_wvCloudOnlyValid || g_wvCloudOnlyNights[n].avgCloudcoverPct < 0) {
      screen.drawString("no data", x, colY + 52);
      continue;
    }

    float pct = g_wvCloudOnlyNights[n].avgCloudcoverPct;
    uint16_t cloudColor;
    if (pct < 25) cloudColor = colorSuccess;
    else if (pct < 50) cloudColor = screen.color565(230, 200, 40);
    else if (pct < 75) cloudColor = screen.color565(230, 130, 40);
    else cloudColor = colorDanger;

    // Hand-drawn percent glyph (two dots + a diagonal stroke), same
    // technique already used for humidity on the Weather page -- this
    // font's charset doesn't render '%' correctly (shows as a fallback
    // "?" glyph), so the number, glyph, and " cloud" text are drawn as
    // three separate pieces with screen.textWidth() tracking the exact
    // x position between them.
    char pctNum[8];
    snprintf(pctNum, sizeof(pctNum), "%.0f", (double)pct);
    screen.setTextSize(2);
    screen.setTextColor(cloudColor, colorBg);
    int cloudX = x;
    int cloudY = colY + 52;
    screen.drawString(pctNum, cloudX, cloudY);
    cloudX += screen.textWidth(pctNum);
    {
      int gx = cloudX + 4;
      int gy = cloudY;
      screen.fillCircle(gx, gy + 3, 2, cloudColor);
      screen.fillCircle(gx + 10, gy + 11, 2, cloudColor);
      screen.drawLine(gx - 1, gy + 13, gx + 11, gy + 1, cloudColor);
    }
    cloudX += 20; // glyph width + gap
    screen.drawString(" cloud", cloudX, cloudY);
  }

  // Clear Sky Chart image -- the classic astronomer's forecast grid,
  // fetched via wv_clearsky_image_service.cpp (GIF source converted to
  // JPEG server-side through wsrv.nl, same established pattern as the
  // SpaceX page's PNG mission photos). Drawn in the space below the
  // 5-day columns that was previously empty.
  {
    // No separate label drawn here -- the chart's own title ("Spruce
    // Knob Mountain Center Clear Sky Chart") is already baked into the
    // image itself, so a duplicate label would be redundant.
    // Left edge moved in from x=20 to x=6 -- at the previous 785px width
    // drawn from x=20, the right edge landed at x=805, past the actual
    // 800px screen width (silently clipped rather than crashing, which is
    // why it looked "maximized" on the right while leaving the left side
    // comparatively empty). Starting further left uses the space evenly
    // on both sides instead.
    // chartY lowered ~1/4in (20px, this project's 80px/in convention)
    // per follow-up feedback -- still leaves a ~10px gap above the
    // diagnostic HTTP line at the image's actual ~145px height.
    int chartY = 276; // raised 20px so the taller image (170->190px cap) grows upward, keeping the same bottom edge rather than pushing toward the screen edge
    // chartX shifts the image right relative to our fixed-x row labels
    // (x=4), widening the label-to-row gap. First-pass estimate (15px)
    // from visual inspection of a device photo, not precisely measured --
    // right edge clips off-screen silently past x=800, same established
    // safe-clipping behavior as the earlier left-edge adjustment above.
    int chartX = 58;
    // Reverted from the reserved-column approach (x=156, 640px image)
    // back to the full-width x=0 layout that was already working well --
    // our own row labels are now drawn overlaid directly on the image
    // instead of in a separate reserved strip.
    screen.setTextSize(1);
    screen.setTextColor(colorDim, colorBg);
    if (g_wvClearSkyImageValid && g_wvClearSkyImagePixels != nullptr) {
      screen.drawRGBBitmap(chartX, chartY, g_wvClearSkyImagePixels, g_wvClearSkyImageWidth, g_wvClearSkyImageHeight);

      // Our own row labels, overlaid on the image (replacing the source
      // chart's illegible embedded ones, now cropped off the source
      // entirely). Y positions are FIRST-PASS ESTIMATES of each row's
      // fraction of the image height, eyeballed from a reference
      // screenshot, not measured precisely. Per follow-up feedback:
      // shifted down 1/4in (20px) overall, plus an extra 4px of
      // separation specifically between Darkness and Smoke (the boundary
      // between the chart's "Sky" and "Ground" row groups).
      const char* rowLabels[9] = {
        "Cloud Cvr", "ECMWF CLD", "Transpncy", "Seeing", "Darkness",
        "Smoke", "Wind", "Humidity", "Temp"
      };
      // Top group (Cloud Cover..Darkness) confirmed equal-height bands
      // with zero gap between them -- uniform 0.070 step matches that
      // exactly. Group gap (Darkness->Smoke) confirmed as ~2 row-heights,
      // so the bottom group continues the same 0.070 step after a
      // 2x (0.140) gap, replacing the earlier ad hoc +4px nudge.
      // Cloud Cover (top group anchor) and Smoke (bottom group anchor)
      // confirmed correct per on-device photo -- held fixed. Every row
      // below each anchor was drifting further down with each step, so
      // the per-row increment was too large; reduced from 0.070 to 0.055
      // in both groups while keeping the anchors themselves unchanged.
      const float rowFracs[9] = {
        0.235f, 0.290f, 0.345f, 0.400f, 0.455f,
        0.655f, 0.710f, 0.765f, 0.820f
      };
      // Exact per-row pixel corrections from on-device photo review,
      // on top of the existing fraction-based estimate -- more precise
      // than re-deriving rowFracs again. Order matches rowLabels[]:
      // Cloud Cover, ECMWF Cloud, Transparency, Seeing, Darkness,
      // Smoke, Wind, Humidity, Temperature.
      const int rowPixelAdjust[9] = {
        -1, 0, 2, 2, 3,
        0, 1, 2, 3
      };
      screen.setTextColor(colorText, colorBg);
      // Group headers -- "SKY" above the top group (Cloud Cvr..Darkness)
      // and "GROUND" above the bottom group (Smoke..Temp), using the
      // same rowFracs/rowPixelAdjust math as the row labels themselves
      // so they stay correctly positioned if those values are ever
      // tuned again. Blue/green reuse colorAccent/colorSuccess (already
      // blue and green respectively) rather than introducing new colors.
      {
        int skyHeaderY = chartY + 20 + (int)(rowFracs[0] * g_wvClearSkyImageHeight) - 6 + rowPixelAdjust[0] - 16;
        int groundHeaderY = chartY + 20 + (int)(rowFracs[5] * g_wvClearSkyImageHeight) - 6 + rowPixelAdjust[5] - 16;
        screen.setTextColor(colorAccent, colorBg);
        screen.drawString("SKY", 4, skyHeaderY);
        screen.drawLine(4, skyHeaderY + 10, 4 + screen.textWidth("SKY"), skyHeaderY + 10, colorAccent);
        screen.setTextColor(colorSuccess, colorBg);
        screen.drawString("GROUND", 4, groundHeaderY);
        screen.drawLine(4, groundHeaderY + 10, 4 + screen.textWidth("GROUND"), groundHeaderY + 10, colorSuccess);
      }
      for (int i = 0; i < 9; i++) {
        int labelY = chartY + 20 + (int)(rowFracs[i] * g_wvClearSkyImageHeight) - 6 + rowPixelAdjust[i];
        screen.setTextColor(colorText, colorBg);
        screen.drawString(rowLabels[i], 4, labelY);
      }
    } else {
      char chartHttpLine[32];
      snprintf(chartHttpLine, sizeof(chartHttpLine), "Chart not loaded (HTTP %d)", g_wvClearSkyImageLastHttpCode);
      screen.drawString(chartHttpLine, 20, chartY);
    }
  }

}

// Formats a trip date as "D Mon YYYY" (e.g. "5 Dec 2026") for on-screen
// display, per follow-up feedback -- storage/JSON still uses plain
// "YYYY-MM-DD" strings (trips_service.cpp), this only affects rendering.
// %d in printf naturally omits any leading zero on the day, unlike
// strftime's %d/%e which always pad -- so the month is fetched via
// strftime("%b") alone and the day/year are assembled with snprintf.
// Uses gmtime() (not localtime()) to match trips_service.cpp's now-
// UTC-based date encoding (daysFromCivil()) -- these are pure calendar
// dates with no real time-of-day/timezone meaning, so both encode and
// decode sides deliberately treat them as UTC throughout, avoiding the
// local-TZ/DST round-tripping that was causing dates to display one
// day early.
static void formatTripDate(time_t unixTime, char* out, size_t outLen) {
  struct tm* ti = gmtime(&unixTime);
  char monthBuf[8];
  strftime(monthBuf, sizeof(monthBuf), "%b", ti);
  snprintf(out, outLen, "%d %s %d", ti->tm_mday, monthBuf, ti->tm_year + 1900);
}

static void draw_world_trips() {
  StateLockGuard lockGuard;
  int centerX = WIDTH / 2;

  screen.setTextDatum(textdatum_t::top_left);

  // Real photo banner (embedded, decoded once at boot -- see
  // world_trips_banner_service.cpp), replacing the earlier hand-drawn
  // sun/palm-tree icon per follow-up feedback. Drawn flush under the
  // top header banner (y=40), full 800px width.
  int bannerY = 40;
  // Top 20px (1/4in at this project's established 80px/in convention)
  // cropped off per follow-up feedback, to reclaim room for the
  // upcoming-trips list below -- done by simply skipping the first 20
  // rows of the already-decoded banner rather than re-cropping/
  // re-embedding all 8 source images again. Cropped from the top rather
  // than the bottom: across most of these 8 images, the main subject
  // (buildings/globe/tiles/icons) sits toward the vertical center-to-
  // bottom of the already-tight crop, often right at the bottom edge,
  // while the top edge is more likely to still have a sliver of
  // background before the busy content starts.
  int cropRows = 20;
  int displayedBannerHeight = g_worldTripsBannerValid ? (g_worldTripsBannerHeight - cropRows) : 0;
  if (displayedBannerHeight < 0) displayedBannerHeight = 0;
  if (g_worldTripsBannerValid && g_worldTripsBannerPixels != nullptr) {
    const uint16_t* croppedPixels = g_worldTripsBannerPixels + (size_t)cropRows * g_worldTripsBannerWidth;
    screen.drawRGBBitmap(0, bannerY, croppedPixels, g_worldTripsBannerWidth, displayedBannerHeight);
  }
  int bannerBottom = bannerY + displayedBannerHeight;

  int nextIdx = trips_service_next_index();
  bool ongoing = (nextIdx >= 0) && trips_service_is_ongoing(nextIdx);

  // Countdown block -- DAYS / HOURS / MINUTES. Vertical spacing
  // compressed from the original design (tuned for the smaller
  // hand-drawn icon) now that the real banner (160px after cropping)
  // takes noticeably more room -- first-pass estimate, likely needs
  // fine-tuning once seen on-device, same as every other page's layout.
  int countdownY = bannerBottom + 25;
  if (nextIdx >= 0 && !ongoing) {
    time_t now = time(nullptr);
    time_t target = g_trips[nextIdx].departUnix;
    uint32_t secsUntil = (target > now) ? (uint32_t)(target - now) : 0;
    int days = secsUntil / 86400;
    int hours = (secsUntil % 86400) / 3600;
    int mins = (secsUntil % 3600) / 60;
    int secs = secsUntil % 60;

    char daysStr[8], hoursStr[8], minsStr[8], secsStr[8];
    snprintf(daysStr, sizeof(daysStr), "%d", days);
    snprintf(hoursStr, sizeof(hoursStr), "%d", hours);
    snprintf(minsStr, sizeof(minsStr), "%d", mins);
    snprintf(secsStr, sizeof(secsStr), "%d", secs);

    // 4 columns now (added Secs per follow-up feedback) -- spacing
    // tightened from 150 to 110 between adjacent columns to fit all 4
    // within the 800px width with room to spare (4*110=440 total span,
    // centered on centerX).
    int colSpacing = 110;
    int col1 = centerX - colSpacing * 3 / 2;
    int col2 = centerX - colSpacing / 2;
    int col3 = centerX + colSpacing / 2;
    int col4 = centerX + colSpacing * 3 / 2;

    screen.setTextSize(4);
    screen.setTextDatum(textdatum_t::middle_center);
    // Days in red (colorDanger, genuinely red in both day/night modes)
    // and Hours in explicit white -- not colorText, since colorText is
    // actually red in night mode (this project's night theme), which
    // would clash with Days rather than contrast against it. Minutes
    // stayed colorAccent (blue) as before. Secs uses colorSuccess
    // (green) as a 4th distinct color -- not specified, picked for
    // variety, adjustable like everything else here.
    //
    // Drop-shadow effect ("3D" look per follow-up feedback): a dark
    // offset copy drawn first, then the real colored number on top --
    // since drawChar/drawString ALWAYS fills its own background
    // rectangle on every call (confirmed in panel_display.cpp -- there
    // is no transparent-text mode in this Canvas API), the main number
    // erases the overlapping part of the shadow, leaving only the
    // offset edge peeking out. Shadow color bumped from (30,30,30) to
    // (90,90,90) -- the original was nearly invisible against this
    // page's near-black background (10,12,16), not distinguishable
    // enough to read as a shadow.
    uint16_t shadowColor = screen.color565(90, 90, 90);
    int shadowOffset = 3;
    screen.setTextColor(shadowColor, colorBg);
    screen.drawString(daysStr, col1 + shadowOffset, countdownY + shadowOffset);
    screen.drawString(hoursStr, col2 + shadowOffset, countdownY + shadowOffset);
    screen.drawString(minsStr, col3 + shadowOffset, countdownY + shadowOffset);
    screen.drawString(secsStr, col4 + shadowOffset, countdownY + shadowOffset);

    screen.setTextColor(colorDanger, colorBg);
    screen.drawString(daysStr, col1, countdownY);
    int daysNumWidth = screen.textWidth(daysStr);
    screen.setTextColor(screen.color565(255, 255, 255), colorBg);
    screen.drawString(hoursStr, col2, countdownY);
    int hoursNumWidth = screen.textWidth(hoursStr);
    screen.setTextColor(colorAccent, colorBg);
    screen.drawString(minsStr, col3, countdownY);
    int minsNumWidth = screen.textWidth(minsStr);
    screen.setTextColor(colorSuccess, colorBg);
    screen.drawString(secsStr, col4, countdownY);
    int secsNumWidth = screen.textWidth(secsStr);

    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    int labelY = countdownY + 30;
    screen.drawString("DAYS", col1, labelY);
    int daysLabelWidth = screen.textWidth("DAYS");
    screen.drawString("HOURS", col2, labelY);
    int hoursLabelWidth = screen.textWidth("HOURS");
    screen.drawString("MINS", col3, labelY);
    int minsLabelWidth = screen.textWidth("MINS");
    screen.drawString("SECS", col4, labelY);
    int secsLabelWidth = screen.textWidth("SECS");

    // Light gray outline box around each column (number + label
    // together), per follow-up feedback. No drawRect outline primitive
    // exists in this project's Canvas API (only fillRect), so the
    // border is built from 4 drawLine calls. Vertical bounds are a
    // first-pass estimate based on this project's ~8px-base-glyph
    // scaling convention (32px tall at size4, 16px tall at size2, with
    // padding) -- adjustable like every other pixel value here once
    // seen on-device. Width per column uses the wider of its number/
    // label text, measured dynamically since "HOURS" is longer than
    // "DAYS"/"MINS"/"SECS".
    uint16_t boxColor = screen.color565(200, 200, 200);
    int boxTop = countdownY - 20;
    int boxBottom = labelY + 14;
    int cols[4] = {col1, col2, col3, col4};
    int numWidths[4] = {daysNumWidth, hoursNumWidth, minsNumWidth, secsNumWidth};
    int labelWidths[4] = {daysLabelWidth, hoursLabelWidth, minsLabelWidth, secsLabelWidth};
    for (int c = 0; c < 4; c++) {
      int boxWidth = (numWidths[c] > labelWidths[c] ? numWidths[c] : labelWidths[c]) + 24;
      int boxLeft = cols[c] - boxWidth / 2;
      int boxRight = cols[c] + boxWidth / 2;
      screen.drawLine(boxLeft, boxTop, boxRight, boxTop, boxColor);
      screen.drawLine(boxLeft, boxBottom, boxRight, boxBottom, boxColor);
      screen.drawLine(boxLeft, boxTop, boxLeft, boxBottom, boxColor);
      screen.drawLine(boxRight, boxTop, boxRight, boxBottom, boxColor);
    }
  } else if (ongoing) {
    screen.setTextSize(3);
    screen.setTextColor(colorSuccess, colorBg);
    screen.setTextDatum(textdatum_t::middle_center);
    screen.drawString("CURRENTLY TRAVELING!", centerX, countdownY);
  } else {
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.setTextDatum(textdatum_t::middle_center);
    screen.drawString("NO UPCOMING TRIPS", centerX, countdownY);
  }

  // Caption -- size2 rather than size3 (was, in the original spacious
  // design) to save vertical room for the trip list below.
  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);
  screen.setTextDatum(textdatum_t::middle_center);
  screen.drawString("UNTIL OUR HOLIDAY", centerX, countdownY + 58);
  screen.setTextDatum(textdatum_t::top_left);

  int midY = countdownY + 85;

  // Next trip details -- combined into 2 lines per follow-up feedback:
  // Line 1 = Dates & Name, Line 2 = Location & Company. Frees up 56px
  // vs. the earlier 4-line version, directly helping the upcoming-trips
  // list below (already tight given the 160px banner).
  if (nextIdx >= 0) {
    Trip& t = g_trips[nextIdx];
    char departDisp[16], returnDisp[16];
    formatTripDate(t.departUnix, departDisp, sizeof(departDisp));
    formatTripDate(t.returnUnix, returnDisp, sizeof(returnDisp));
    char datesStr[48];
    snprintf(datesStr, sizeof(datesStr), "%s - %s   ", departDisp, returnDisp);
    char line2[64];
    snprintf(line2, sizeof(line2), "%s   %s", t.location.c_str(), t.company.c_str());

    screen.setTextSize(2);
    // Line 1 split into two colored segments (dates in accent, name in
    // red) rather than one drawString call, per follow-up feedback --
    // this project's textdatum_t only offers top_left/top_right/
    // middle_center (no middle_left), so horizontal centering is done
    // manually via textWidth() and a -8px vertical offset (half this
    // project's ~16px size2 line height) replicates middle_center's
    // vertical centering while still allowing two different colors on
    // one line. First-pass vertical offset estimate, adjustable like
    // every other pixel value in this project.
    screen.setTextDatum(textdatum_t::top_left);
    int datesWidth = screen.textWidth(datesStr);
    int nameWidth = screen.textWidth(t.name.c_str());
    int line1StartX = centerX - (datesWidth + nameWidth) / 2;
    int line1Y = midY - 8;
    screen.setTextColor(colorAccent, colorBg);
    screen.drawString(datesStr, line1StartX, line1Y);
    screen.setTextColor(colorDanger, colorBg);
    // Fake bold -- drawn twice with a 1px horizontal offset, since this
    // project's custom bitmap font has no true bold variant.
    screen.drawString(t.name.c_str(), line1StartX + datesWidth, line1Y);
    screen.drawString(t.name.c_str(), line1StartX + datesWidth + 1, line1Y);
    midY += 28;

    screen.setTextColor(colorDim, colorBg);
    screen.setTextDatum(textdatum_t::middle_center);
    screen.drawString(line2, centerX, midY);
    midY += 28;
    screen.setTextDatum(textdatum_t::top_left);
  } else {
    midY += 28;
  }

  // Future trips list -- everything else in g_trips within 3 years,
  // smaller font, sorted soonest-first, excluding whichever trip is
  // already shown as "next" above. Simple insertion sort of indices --
  // TRIPS_MAX (20) is small enough that this is negligible cost, and it
  // means trips can be added to the JSON file in any order.
  //
  // NOTE: with the real banner taking 160px (vs. the earlier small
  // hand-drawn icon), there's only room for roughly 1-2 list entries
  // below the "next trip" details before hitting the bottom of the
  // screen -- the listY < HEIGHT-20 loop bound below handles this
  // gracefully (list just truncates) rather than overflowing off-screen.
  {
    int listY = midY - 9;
    screen.setTextSize(1);
    screen.setTextColor(colorAccent, colorBg);
    screen.drawString("UPCOMING TRIPS", 30, listY);
    // White separator line (was a blue/colorAccent underline) -- matches
    // the same style as the between-entry dividers below, but white to
    // stand apart from them.
    screen.drawLine(30, listY + 14, WIDTH - 30, listY + 14, screen.color565(255, 255, 255));
    listY += 22;

    time_t now = time(nullptr);
    time_t threeYearsOut = now + (3L * 365L * 86400L);

    int order[TRIPS_MAX];
    int orderCount = 0;
    for (int i = 0; i < g_tripsCount; i++) {
      if (i == nextIdx) continue;
      if (g_trips[i].returnUnix < now) continue;
      if (g_trips[i].departUnix > threeYearsOut) continue;
      order[orderCount++] = i;
    }
    for (int a = 1; a < orderCount; a++) {
      int key = order[a];
      int b = a - 1;
      while (b >= 0 && g_trips[order[b]].departUnix > g_trips[key].departUnix) {
        order[b + 1] = order[b];
        b--;
      }
      order[b + 1] = key;
    }

    screen.setTextSize(1);
    for (int i = 0; i < orderCount && listY < HEIGHT - 20; i++) {
      Trip& t = g_trips[order[i]];
      char departDisp[16], returnDisp[16];
      formatTripDate(t.departUnix, departDisp, sizeof(departDisp));
      formatTripDate(t.returnUnix, returnDisp, sizeof(returnDisp));
      char line[128];
      snprintf(line, sizeof(line), "%s to %s / %s / %s / %s",
               departDisp, returnDisp, t.name.c_str(), t.location.c_str(), t.company.c_str());
      // Alternating row colors (white / blue) per follow-up feedback --
      // explicit white rather than colorText, which is actually red in
      // night mode (same reasoning as the Hours countdown number).
      uint16_t rowColor = (i % 2 == 0) ? screen.color565(255, 255, 255) : colorAccent;
      screen.setTextColor(rowColor, colorBg);
      screen.drawString(line, 30, listY);
      listY += 14;

      // Divider between entries -- omitted after the last one so the
      // list doesn't end with a trailing line.
      if (i < orderCount - 1) {
        screen.drawLine(30, listY, WIDTH - 30, listY, colorDim);
        listY += 8;
      }
    }
    if (orderCount == 0) {
      screen.setTextColor(colorDim, colorBg);
      screen.drawString("No further trips scheduled", 30, listY);
    }
  }
}

void screen_manager_draw() {
  g_nightModeActive = computeNightModeActive();

  {
    time_t nowUnix = time(nullptr);
    bool inDimWindow = false;
    if (nowUnix > 100000) {
      struct tm* ti = localtime(&nowUnix);
      inDimWindow = (ti->tm_hour >= 22 || ti->tm_hour < 8);
    }
    if (g_inDimWindowActive && !inDimWindow) {
      g_dimOverrideActive = false; // window just ended -- start the next night back in the dimmed state
    } else if (!g_inDimWindowActive && inDimWindow) {
      g_dimOverrideActive = false; // window just started -- force dim regardless of any
                                    // daytime toggle that may have been left set
    }
    g_inDimWindowActive = inDimWindow;
  }
  colorBg = g_nightModeActive ? colorBgNight : colorBgDay;
  colorText = g_nightModeActive ? colorTextNight : colorTextDay;
  colorDim = g_nightModeActive ? colorDimNight : colorDimDay;
  colorAccent = g_nightModeActive ? colorAccentNight : colorAccentDay;
  colorSuccess = g_nightModeActive ? colorSuccessNight : colorSuccessDay;
  colorDanger = g_nightModeActive ? colorDangerNight : colorDangerDay;
  colorStarship = g_nightModeActive ? colorStarshipNight : colorStarshipDay;

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
    case 3: draw_spacex(); break;
    case 4: draw_iss(); break;
    case 5: draw_weather(); break;
    case 6: draw_imagery(); break;
    case 7: draw_trends(); break;
    case 8: draw_wv_astro(); break;
    case 9: draw_world_trips(); break;
  }

  // Shown on the Dashboard (tab 0) only, per follow-up feedback --
  // previously shown on every page except WV Astro (tab 8); now hidden
  // everywhere except Dashboard instead.
  if (currentTab == 0) {
    screen.setTextSize(1);
    screen.setTextColor(colorDim, colorBg);
    screen.setTextDatum(textdatum_t::top_right);
    screen.drawString(FIRMWARE_VERSION, WIDTH - 6, HEIGHT - 14);
  }

  if (g_nightModeActive) {
    screen.setTextColor(colorDim, colorBg);
    screen.setTextDatum(textdatum_t::top_left);
    screen.drawString("NIGHT", 10, HEIGHT - 14);
  }

  // LOCKED and BRIGHT/DIMMED moved into the banner itself (see
  // drawHeader()), centered rather than bottom-right.

  checkAlertTriggers();
  drawAlertBanner();

  // Auto-dim: 10pm-8am, unless a bottom-to-top swipe toggled the override
  // active (g_dimOverrideActive, computed once per frame above alongside
  // g_inDimWindowActive). Dims the just-drawn frame in place, right before
  // it's presented -- see Canvas::dimFrameBuffer() for why this is done at
  // the buffer level instead of true backlight PWM (not available on this
  // board's wiring).
  if (g_inDimWindowActive && !g_dimOverrideActive) {
    screen.dimFrameBuffer();
  }
}

static uint16_t lastTouchX = 0;
static uint16_t lastTouchY = 0;

static const int AVIATION_TAB_INDEX = 1;
static const int IMAGERY_TAB_INDEX = 6;
static const int WORLD_TRIPS_TAB_INDEX = 9;

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
    // Advances to the next queued alert (if any) rather than just
    // clearing, so a dismiss never skips a still-pending alert.
    advanceAlertQueue();
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

    bool isVerticalSwipeUp = upExcursion >= VERTICAL_SWIPE_MIN_PX &&
                             upExcursion > horizontalPeak &&
                             upExcursion > downExcursion;

    if (isVerticalSwipeDown) {
      // Top-to-bottom swipe toggles the page lock -- the current page
      // keeps drawing and updating live data normally, it just stops
      // advancing to the next tab (via swipe, tap-to-advance, or idle
      // auto-cycle) until swiped down again.
      g_pageLocked = !g_pageLocked;
    } else if (isVerticalSwipeUp) {
      // Bottom-to-top swipe toggles the auto-dim override, regardless of
      // page lock -- not page navigation, same category as the
      // night-mode long-press. Persists until swiped again (no timer),
      // replacing the old 5-minute temporary wake.
      g_dimOverrideActive = !g_dimOverrideActive;
    } else if (isHorizontalSwipe) {
      // Horizontal swipe always goes back a page now, regardless of
      // direction (left or right) -- suppressed while page-locked, same
      // as before (the point of the lock is to stay put until it's
      // explicitly unlocked).
      if (!g_pageLocked) {
        currentTab = (currentTab - 1 + TAB_COUNT) % TAB_COUNT;
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
      // Hidden test buttons disabled -- this page is now the Imagery
      // gallery, not Debug, so these dev-only triggers (bounce-buffer
      // cycle test, aviation poll-interval cycle test, alert banner test)
      // no longer have a home here. Kept as always-false rather than
      // removed outright, so the consuming if/else-if chain below (which
      // also handles tap-to-advance) doesn't need restructuring.
      bool hitNextButton = false;
      bool hitPollButton = false;
      bool hitAlertTestButton = false;

      // Hidden hotspot in the banner's clear middle section (between the
      // "IMAGERY" title on the left and TAP>/<SWIPE/WiFi icon on the
      // right) forces a new random image immediately, bypassing the
      // normal 15-min rotation timer (see imagery_service.cpp).
      bool hitImageryHotspot = currentTab == IMAGERY_TAB_INDEX &&
                                lastTouchX >= 150 && lastTouchX <= 650 &&
                                lastTouchY >= 0 && lastTouchY <= 40;
      if (hitImageryHotspot) {
        imagery_update();
      }

      // Tap-to-cycle hotspot over the World Trips banner image itself
      // (y=40 to y=200, full width) -- lets the images be checked one by
      // one on demand instead of waiting for the 3-hour rotation timer,
      // same "manual override" idea as the Imagery page's hotspot above,
      // just placed over the visible image rather than a hidden banner
      // strip since there's no separate title-bar clear space to use here.
      bool hitWorldTripsHotspot = currentTab == WORLD_TRIPS_TAB_INDEX &&
                                   lastTouchY >= 40 && lastTouchY <= 200;
      if (hitWorldTripsHotspot) {
        world_trips_banner_update();
      }

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
        enqueueAlert("TEST ALERT - HIDDEN DEBUG TRIGGER", colorDanger);
      } else if (hitImageryHotspot) {
        // Already handled above -- just prevents falling through to
        // tap-to-advance.
      } else if (hitWorldTripsHotspot) {
        // Same as hitImageryHotspot above -- already handled, just
        // prevents this tap from also advancing to the next tab.
      } else if (!handledAviation && !g_pageLocked) {
        currentTab = (currentTab + 1) % TAB_COUNT;
      }
    }
  }
  touchWasDown = touched;
}
