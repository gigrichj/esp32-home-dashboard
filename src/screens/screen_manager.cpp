#include "screen_manager.h"
#include "../panel_display.h"
#include "../version.h"
#include "../services/weather_service.h"
#include "../services/iss_service.h"
#include "../services/aviation_service.h"
#include "../services/air_quality_service.h"
#include "secrets.h"
#include "../debug_log.h"
#include "../debug_controls.h"
#include <math.h>
#include <time.h>
#include <WiFi.h>

using namespace PanelDisplay;

static const char* TAB_NAMES[] = {
  "DASHBOARD", "AVIATION", "PORSCHE", "ISS", "WEATHER", "DEBUG"
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
static int currentTab = 0;

static uint16_t colorBg;
static uint16_t colorSuccess;
static uint16_t colorDanger;
static uint16_t colorText;
static uint16_t colorDim;
static uint16_t colorAccent;

static bool touchWasDown = false;
static uint32_t touchDownMs = 0;
static const uint32_t TAP_MIN_MS = 50;
static const uint32_t TAP_MAX_MS = 600;

static String formatCurrentDateTime() {
  time_t now = time(nullptr);
  if (now < 100000) return String("Time syncing...");
  struct tm* t = localtime(&now);
  char buf[40];
  strftime(buf, sizeof(buf), "%a, %b %d  %I:%M %p", t);
  return String(buf);
}

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

static void drawCloudIcon(int cx, int cy, int r, uint16_t color); // defined further down, used here

static void drawDashboardBackground() {
  uint32_t t = millis();

  // Cloud drift low on the screen, echoing whatever the weather page is
  // currently showing, so the dashboard feels alive without being busy.
  if (g_weather.valid && g_weather.weatherId > 800) {
    uint16_t cloudColor = screen.color565(22, 26, 32);
    for (int i = 0; i < 4; i++) {
      int driftSpan = WIDTH + 200;
      uint32_t speed = 35 + (uint32_t)(i % 2) * 15;
      int x = (int)((t / speed + (uint32_t)i * 220) % (uint32_t)driftSpan) - 100;
      int y = 320 + (i % 2) * 50;
      drawCloudIcon(x, y, 26 + (i % 2) * 8, cloudColor);
    }
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
    int y = 190;
    screen.fillRect(x - 3, y - 3, 6, 6, issColor);
    screen.fillRect(x - 15, y - 2, 9, 4, issColor);
    screen.fillRect(x + 6, y - 2, 9, 4, issColor);
  }
}

static int countVisibleAircraft(); // defined further down, used in draw_dashboard()

static void draw_dashboard() {
  drawDashboardBackground();

  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.setTextDatum(textdatum_t::top_left);
  screen.drawString(formatCurrentDateTime(), 20, 55);
  screen.setTextColor(colorText, colorBg);

  // WiFi status - small, tucked in the top-right corner instead of the
  // main list, so it doesn't compete for attention with the real data.
  screen.setTextSize(1);
  screen.setTextDatum(textdatum_t::top_right);
  if (WiFi.status() == WL_CONNECTED) {
    char line[64];
    snprintf(line, sizeof(line), "WiFi: %s (%s)", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    screen.setTextColor(colorSuccess, colorBg);
    screen.drawString(line, WIDTH - 10, 50);
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
  int y = 240;
  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString("WEATHER", leftX, y);
  screen.drawLine(leftX, y + 20, leftX + 84, y + 20, colorAccent);
  y += 30;

  screen.setTextSize(2);
  screen.setTextColor(colorText, colorBg);
  if (g_weather.valid) {
    snprintf(line, sizeof(line), "%.0fF  %s", g_weather.tempF, g_weather.condition.c_str());
    screen.drawString(line, leftX, y);
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

  {
    // Cross-page teaser: countdown to the next sunrise/sunset, so the
    // dashboard hints at the weather page's data without duplicating it.
    if (g_weather.valid && g_weather.sunriseUnix > 0 && g_weather.sunsetUnix > 0) {
      uint32_t nowUnix = (uint32_t)time(nullptr);
      bool isDay = nowUnix >= g_weather.sunriseUnix && nowUnix < g_weather.sunsetUnix;
      uint32_t targetUnix = isDay ? g_weather.sunsetUnix : g_weather.sunriseUnix;
      uint32_t secsUntil = (targetUnix > nowUnix) ? (targetUnix - nowUnix) : 0;
      int hh = secsUntil / 3600;
      int mm = (secsUntil % 3600) / 60;
      char teaser[48];
      snprintf(teaser, sizeof(teaser), "%s in %dh %dm", isDay ? "Sunset" : "Sunrise", hh, mm);
      screen.setTextSize(1);
      screen.setTextColor(colorDim, colorBg);
      screen.drawString(teaser, leftX, y);
    }
  }

  // AIRCRAFT + ISS column, pushed to the right so this reads as a second
  // column rather than continuing the same vertical list.
  int y2 = 240;
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
      screen.setTextSize(1);
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
    int tickLen = constrain(a.altitudeFt / 1000, 2, 14);
    screen.drawLine(px, py + 5, px, py + 5 + tickLen, planeColor);

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

static String formatHHMM(uint32_t unixTime) {
  if (unixTime == 0) return String("--:--");
  time_t t = (time_t)unixTime;
  struct tm* timeInfo = localtime(&t);
  char buf[16];
  strftime(buf, sizeof(buf), "%I:%M %p", timeInfo);
  return String(buf);
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
    return;
  }

  bool isNight = false;
  time_t now = time(nullptr);
  if (now > 100000 && g_weather.sunriseUnix > 0 && g_weather.sunsetUnix > 0) {
    isNight = (uint32_t)now < g_weather.sunriseUnix || (uint32_t)now > g_weather.sunsetUnix;
  }
  drawWeatherBackground(g_weather.weatherId, isNight);

  drawWeatherIcon(80, 100, 40, g_weather.weatherId, colorText);

  screen.setTextSize(4);
  screen.setTextColor(colorText, colorBg);
  char tempStr[16];
  snprintf(tempStr, sizeof(tempStr), "%.0fF", g_weather.tempF);
  screen.drawString(tempStr, 150, 70);

  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString(g_weather.condition.c_str(), 150, 130);

  int y = 170;
  screen.setTextSize(2);
  char row[48];

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Feels like", 20, y);
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%.0fF", g_weather.feelsLikeF);
  screen.drawString(row, 260, y);
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
  screen.setTextColor(colorText, colorBg);
  snprintf(row, sizeof(row), "%.0fF", g_weather.dewPointF);
  screen.drawString(row, 260, y);
  y += 30;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Sunrise", 20, y);
  screen.setTextColor(colorText, colorBg);
  screen.drawString(formatHHMM(g_weather.sunriseUnix), 260, y);
  y += 30;

  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Sunset", 20, y);
  screen.setTextColor(colorText, colorBg);
  screen.drawString(formatHHMM(g_weather.sunsetUnix), 260, y);

  {
    // Precipitation gauge: a 270-degree arc (gap at the bottom), approximated
    // with short line segments since this display library doesn't expose a
    // drawArc primitive. A blue segment fills in up to the current percent.
    int gaugeCx = 575, gaugeCy = 268, gaugeR = 22;
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

    screen.setTextSize(1);
    screen.setTextColor(colorDim, colorBg);
    screen.setTextDatum(textdatum_t::top_left);
    screen.drawString("PRECIP", gaugeCx - 18, gaugeCy + gaugeR + 12);

    screen.setTextSize(2);
    screen.setTextColor(colorText, colorBg);
    char precipStr[8];
    snprintf(precipStr, sizeof(precipStr), "%d", g_weather.precipChance);
    screen.drawString(precipStr, gaugeCx - 12, gaugeCy + gaugeR + 30);
    {
      // Hand-drawn percent glyph, same trick used for humidity above.
      int gx = gaugeCx + 8;
      int gy = gaugeCy + gaugeR + 36;
      screen.fillCircle(gx, gy - 5, 2, colorText);
      screen.fillCircle(gx + 8, gy + 3, 2, colorText);
      screen.drawLine(gx - 1, gy + 5, gx + 9, gy - 7, colorText);
    }
    screen.setTextDatum(textdatum_t::top_left);
  }

  {
    // Wind compass: direction needle plus sustained | gust speeds below.
    int windCx = 695, windCy = 268, windR = 22;
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

    screen.setTextSize(1);
    screen.setTextColor(colorDim, colorBg);
    screen.setTextDatum(textdatum_t::top_left);
    screen.drawString("WINDS", windCx - 15, windCy + windR + 12);

    screen.setTextSize(2);
    screen.setTextColor(colorText, colorBg);
    char windStr[24];
    snprintf(windStr, sizeof(windStr), "%.0f / %.0f", g_weather.windMph, g_weather.windGustMph);
    // Center dynamically based on actual string length, since digit count
    // varies (e.g. "1 / 3" vs "12 / 18") -- this font is monospace-ish at
    // roughly 12px/char at text size 2.
    int windStrWidth = (int)strlen(windStr) * 12;
    screen.drawString(windStr, windCx - windStrWidth / 2, windCy + windR + 30);
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

      int barW = 200, barH = 14, segGap = 4;
      int segW = barW / 5;
      for (int s = 0; s < 5; s++) {
        uint16_t drawColor = (s < g_airQuality.aqi) ? airQualityColor(s + 1) : colorDim;
        screen.fillRect(aqX + s * segW, aqY, segW - segGap, barH, drawColor);
      }
      aqY += barH + 24;

      screen.setTextSize(1);
      screen.setTextColor(colorDim, colorBg);
      char pmLine[32];
      snprintf(pmLine, sizeof(pmLine), "PM2.5: %.1f ug/m3", g_airQuality.pm2_5);
      screen.drawString(pmLine, aqX, aqY);
      aqY += 20;
      snprintf(pmLine, sizeof(pmLine), "PM10: %.1f ug/m3", g_airQuality.pm10);
      screen.drawString(pmLine, aqX, aqY);
    } else {
      screen.setTextSize(2);
      screen.setTextColor(colorDim, colorBg);
      screen.drawString("--", aqX, aqY);
    }
    screen.setTextSize(2);
    screen.setTextColor(colorText, colorBg);
  }

  int stripY = 360;
  screen.drawLine(20, stripY - 10, WIDTH - 20, stripY - 10, colorDim);

  int colW = (WIDTH - 40) / 5;
  screen.setTextDatum(textdatum_t::middle_center);
  for (int i = 0; i < g_forecastCount; i++) {
    int cx = 20 + colW * i + colW / 2;

    screen.setTextSize(1);
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
static String formatPassTime(uint32_t unixTime) {
  if (unixTime == 0) return "--";
  time_t t = (time_t)unixTime;
  struct tm* ti = localtime(&t);
  int h12 = ti->tm_hour % 12;
  if (h12 == 0) h12 = 12;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d%s", h12, ti->tm_min, ti->tm_hour < 12 ? "A" : "P");
  return String(buf);
}

// Compact date like "Jul18" for the visible-passes list.
static String formatPassDate(uint32_t unixTime) {
  if (unixTime == 0) return "--";
  time_t t = (time_t)unixTime;
  struct tm* ti = localtime(&t);
  char buf[16];
  strftime(buf, sizeof(buf), "%b%d", ti);
  return String(buf);
}

static void drawIssIcon(int cx, int cy, uint16_t color) {
  screen.fillRect(cx - 3, cy - 3, 6, 6, color);
  screen.fillRect(cx - 15, cy - 2, 9, 4, color);
  screen.fillRect(cx + 6, cy - 2, 9, 4, color);
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
  }

  int issX = MAP_X + (int)((g_iss.lon + 180) / 360.0f * MAP_W);
  int issY = MAP_Y + (int)((90 - g_iss.lat) / 180.0f * MAP_H);
  drawIssIcon(issX, issY, colorIss);

  screen.setTextSize(1);
  screen.setTextColor(colorIss, colorBg);
  char posLabel[32];
  snprintf(posLabel, sizeof(posLabel), "%.2f, %.2f", g_iss.lat, g_iss.lon);
  screen.drawString(posLabel, issX + 16, issY - 6);

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
  screen.drawString(row, col1X + 110, contentY + 32);

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
    screen.setTextSize(1);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("Duration", col2X, contentY + 32);
    screen.setTextColor(colorText, colorBg);
    snprintf(row, sizeof(row), "%d min", g_iss.nextPassDurationSec / 60);
    screen.drawString(row, col2X + 70, contentY + 32);
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
    screen.setTextSize(1);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("Duration", col2X, detailY);
    screen.setTextColor(colorText, colorBg);
    snprintf(row, sizeof(row), "%d min", g_iss.nextPassDurationSec / 60);
    screen.drawString(row, col2X + 70, detailY);

    if (g_issPassCount > 0) {
      snprintf(row, sizeof(row), "Max El %d  Look %s", g_issPasses[0].maxElevationDeg,
               g_issPasses[0].maxAzCompass.c_str());
      screen.drawString(row, col2X, detailY + 16);
    }

    snprintf(row, sizeof(row), "Crew Aboard %d", g_issCrewCount);
    screen.drawString(row, col2X, detailY + 32);
  } else {
    screen.setTextSize(2);
    screen.setTextColor(colorDim, colorBg);
    screen.drawString("No upcoming pass found", col2X, contentY);
  }

  // Column 3: Visible Passes list -- up to 3 rows, columns aligned via
  // fixed-width padding (this font is monospace, so %-Ns keeps every
  // row's DATE/START/END/EL/MAG lined up under one another).
  {
    screen.setTextSize(1);
    int rowY = contentY;

    screen.setTextColor(colorDim, colorBg);
    screen.drawString("DATE  START  END    EL  MAG", col3X, rowY);
    rowY += 18;

    int shownPasses = min(g_issPassCount, 3);
    if (shownPasses == 0) {
      screen.setTextColor(colorDim, colorBg);
      screen.drawString("No passes in the", col3X, rowY);
      screen.drawString("next few days", col3X, rowY + 16);
    } else {
      for (int i = 0; i < shownPasses; i++) {
        IssPass& p = g_issPasses[i];
        char line[64];
        snprintf(line, sizeof(line), "%-6s%-7s%-7s%-4d%.1f",
                 formatPassDate(p.startUnix).c_str(),
                 formatPassTime(p.startUnix).c_str(),
                 formatPassTime(p.endUnix).c_str(),
                 p.maxElevationDeg,
                 p.magnitude);
        screen.setTextColor(colorText, colorBg);
        screen.drawString(line, col3X, rowY);
        rowY += 18;
      }
    }
  }
}

static void draw_debug() {
  screen.setTextDatum(textdatum_t::top_left);

  char bbLine[48];
  snprintf(bbLine, sizeof(bbLine), "Bounce buffer: %d lines", PanelDisplay::getBounceBufferLines());
  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString(bbLine, 10, 50);
  screen.setTextSize(1);
  screen.setTextColor(colorDim, colorBg);
  screen.drawString("Tap NEXT to save the next test value and reboot", 10, 78);

  screen.fillRect(600, 400, 180, 60, colorAccent);
  screen.setTextColor(colorBg, colorAccent);
  screen.setTextSize(2);
  screen.setTextDatum(textdatum_t::middle_center);
  screen.drawString("NEXT >>", 690, 430);
  screen.setTextDatum(textdatum_t::top_left);

  char pollLine[48];
  snprintf(pollLine, sizeof(pollLine), "Aviation poll: %lus", (unsigned long)(g_aviationPollMs / 1000));
  screen.setTextSize(2);
  screen.setTextColor(colorAccent, colorBg);
  screen.drawString(pollLine, 10, 320);
  screen.fillRect(230, 310, 180, 60, colorAccent);
  screen.setTextColor(colorBg, colorAccent);
  screen.setTextDatum(textdatum_t::middle_center);
  screen.drawString("NEXT >>", 320, 340);
  screen.setTextDatum(textdatum_t::top_left);

  screen.setTextSize(1);
  screen.setTextColor(colorText, colorBg);
  int y = 105;
  for (int i = 0; i < DEBUG_LOG_LINES; i++) {
    if (g_debugLog[i].length() > 0) {
      screen.drawString(g_debugLog[i], 10, y);
    }
    y += 18;
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
  colorSuccess = screen.color565(80, 200, 120);
  colorDanger = screen.color565(220, 80, 80);
}

void screen_manager_draw() {
  screen.fillScreen(colorBg);
  drawHeader();

  switch (currentTab) {
    case 0: draw_dashboard(); break;
    case 1: draw_aviation(); break;
    case 2: draw_placeholder("Porsche"); break;
    case 3: draw_iss(); break;
    case 4: draw_weather(); break;
    case 5: draw_debug(); break;
  }

  screen.setTextSize(1);
  screen.setTextColor(colorDim, colorBg);
  screen.setTextDatum(textdatum_t::top_right);
  screen.drawString(FIRMWARE_VERSION, WIDTH - 6, HEIGHT - 14);
}

static const int DEBUG_TAB_INDEX = 5;
static uint16_t lastTouchX = 0;
static uint16_t lastTouchY = 0;

static const int AVIATION_TAB_INDEX = 1;

void screen_manager_handle_touch(bool touched, uint16_t x, uint16_t y) {
  uint32_t now = millis();
  if (touched) {
    // Remember the position while the finger is actually down - on
    // release, readTouch() reports no point and x/y come in as 0,0,
    // so we can't rely on the release-time coordinates directly.
    lastTouchX = x;
    lastTouchY = y;
    if (!touchWasDown) {
      touchDownMs = now;
    }
  }
  if (!touched && touchWasDown) {
    uint32_t held = now - touchDownMs;
    if (held >= TAP_MIN_MS && held <= TAP_MAX_MS) {
      bool hitNextButton = currentTab == DEBUG_TAB_INDEX &&
                            lastTouchX >= 600 && lastTouchX <= 780 &&
                            lastTouchY >= 400 && lastTouchY <= 460;
      bool hitPollButton = currentTab == DEBUG_TAB_INDEX &&
                            lastTouchX >= 230 && lastTouchX <= 410 &&
                            lastTouchY >= 310 && lastTouchY <= 370;

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
      } else if (!handledAviation) {
        currentTab = (currentTab + 1) % TAB_COUNT;
      }
    }
  }
  touchWasDown = touched;
}
