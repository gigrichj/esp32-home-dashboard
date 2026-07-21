#include "astro_seeing_service.h"
#include "wifi_manager.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

AstroForecastPoint g_astroForecast[ASTRO_MAX_POINTS];
int g_astroForecastCount = 0;
int g_astroLastHttpCode = -999;

float g_moonPhaseFraction = 0;
float g_moonIllumPercent = 0;
String g_moonPhaseLabel = "--";

// Wording now mirrors the on-screen GOOD/FAIR/POOR/BAD color key exactly --
// same index thresholds as astroSeverityColor() in screen_manager.cpp, so a
// given index always shows a word that matches its color band.
const char* astro_seeing_label(int idx) {
  if (idx <= 2) return "GOOD";
  if (idx <= 4) return "FAIR";
  if (idx <= 6) return "POOR";
  if (idx <= 8) return "BAD";
  return "--";
}

const char* astro_transparency_label(int idx) {
  if (idx <= 2) return "GOOD";
  if (idx <= 4) return "FAIR";
  if (idx <= 6) return "POOR";
  if (idx <= 8) return "BAD";
  return "--";
}

const char* astro_cloudcover_label(int idx) {
  // Cloud cover's color scale uses a max of 9 (not 8), so the thresholds
  // are scaled up proportionally to land on the same 4 color bands.
  if (idx <= 2) return "GOOD";
  if (idx <= 4) return "FAIR";
  if (idx <= 6) return "POOR";
  if (idx <= 9) return "BAD";
  return "--";
}

const char* astro_instability_label(int liftedIndex) {
  if (liftedIndex > 0) return "Stable";
  if (liftedIndex > -4) return "Slight Risk";
  if (liftedIndex > -8) return "Moderate Risk";
  return "High Risk";
}

const char* astro_tonight_verdict(int cloudcover, int seeing, int transparency,
                                   float moonIllumPercent, float* outBadness) {
  float cloudFrac = (float)(cloudcover - 1) / 8.0f;    // cloudcover is 1-9
  float seeingFrac = (float)(seeing - 1) / 7.0f;       // seeing is 1-8
  float transFrac = (float)(transparency - 1) / 7.0f;  // transparency is 1-8
  float moonFrac = moonIllumPercent / 100.0f;

  float badness = (cloudFrac * 3.0f + seeingFrac * 2.0f + transFrac * 1.0f + moonFrac * 1.0f) / 7.0f;
  if (outBadness) *outBadness = badness;

  if (badness < 0.2f) return "Excellent Night";
  if (badness < 0.4f) return "Good Night";
  if (badness < 0.6f) return "Fair Night";
  if (badness < 0.8f) return "Poor Night";
  return "Skip Tonight";
}

// ESP32's toolchain doesn't provide timegm(), and mktime() assumes local
// time (this device runs on EST5EDT, not UTC) -- so 7Timer's UTC "init"
// timestamp needs a timezone-independent manual conversion instead.
// This is the standard "days from civil" algorithm (Howard Hinnant),
// correct for the Gregorian calendar.
static uint32_t utcTmToUnix(int year, int month, int day, int hour) {
  int y = year;
  int m = month;
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  int yoe = (int)(y - era * 400);
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long daysSinceEpoch = era * 146097L + doe - 719468L;
  return (uint32_t)(daysSinceEpoch * 86400L + (long)hour * 3600L);
}

static void computeMoonPhase(uint32_t nowUnix) {
  const double synodicDays = 29.530588;
  // Known new moon reference: 2000-01-06 18:14 UTC
  const double refNewMoonUnix = 947182440.0;
  double daysSince = ((double)nowUnix - refNewMoonUnix) / 86400.0;
  double phaseDays = fmod(daysSince, synodicDays);
  if (phaseDays < 0) phaseDays += synodicDays;
  double frac = phaseDays / synodicDays;
  double illum = (1.0 - cos(2.0 * PI * frac)) / 2.0 * 100.0;

  g_moonPhaseFraction = (float)frac;
  g_moonIllumPercent = (float)illum;

  if (frac < 0.03 || frac > 0.97)      g_moonPhaseLabel = "New Moon";
  else if (frac < 0.22)                g_moonPhaseLabel = "Waxing Crescent";
  else if (frac < 0.28)                g_moonPhaseLabel = "First Quarter";
  else if (frac < 0.47)                g_moonPhaseLabel = "Waxing Gibbous";
  else if (frac < 0.53)                g_moonPhaseLabel = "Full Moon";
  else if (frac < 0.72)                g_moonPhaseLabel = "Waning Gibbous";
  else if (frac < 0.78)                g_moonPhaseLabel = "Last Quarter";
  else                                  g_moonPhaseLabel = "Waning Crescent";
}

void astro_recompute_moon_phase() {
  uint32_t nowUnix = (uint32_t)time(nullptr);
  if (nowUnix > 100000) {
    computeMoonPhase(nowUnix);
  }
}

void astro_seeing_service_update() {
  astro_recompute_moon_phase();

  if (!wifi_manager_is_connected()) return;

  HTTPClient http;
  char url[192];
  snprintf(url, sizeof(url),
    "https://www.7timer.info/bin/api.pl?lon=%f&lat=%f&product=astro&output=json",
    (double)HOME_LON, (double)HOME_LAT);

  http.begin(url);
  http.setTimeout(15000); // 7Timer is a small community-run service and
                          // can be slow to respond -- the default timeout
                          // was likely cutting it off before it replied.
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); // 7Timer returns a
                          // 302 redirect (likely to a mirror server) --
                          // HTTPClient doesn't follow redirects by default.
  http.addHeader("User-Agent", "ESP32-Home-Dashboard/1.0");
  int code = http.GET();
  g_astroLastHttpCode = code;
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      const char* initStr = doc["init"] | "";
      uint32_t initUnix = 0;
      if (strlen(initStr) >= 10) {
        char yearBuf[5] = {0}, monBuf[3] = {0}, dayBuf[3] = {0}, hourBuf[3] = {0};
        memcpy(yearBuf, initStr, 4);
        memcpy(monBuf, initStr + 4, 2);
        memcpy(dayBuf, initStr + 6, 2);
        memcpy(hourBuf, initStr + 8, 2);
        int year  = atoi(yearBuf);
        int month = atoi(monBuf);
        int day   = atoi(dayBuf);
        int hour  = atoi(hourBuf);
        initUnix = utcTmToUnix(year, month, day, hour);
      }

      JsonArray series = doc["dataseries"].as<JsonArray>();
      g_astroForecastCount = 0;
      for (JsonObject p : series) {
        if (g_astroForecastCount >= ASTRO_MAX_POINTS) break;
        int timepoint = p["timepoint"] | 0;
        AstroForecastPoint& pt = g_astroForecast[g_astroForecastCount];
        pt.unixTime     = initUnix + (uint32_t)timepoint * 3600UL;
        pt.cloudcover   = p["cloudcover"]   | 0;
        pt.seeing       = p["seeing"]       | 0;
        pt.transparency = p["transparency"] | 0;
        pt.liftedindex  = p["liftedindex"]  | 0;
        const char* prectype = p["prectype"] | "none";
        pt.prectype = String(prectype);
        g_astroForecastCount++;
      }
    } else {
      Serial.printf("[Astro] JSON parse error: %s\n", err.c_str());
      Serial.printf("[Astro] raw payload: %s\n", payload.c_str());
    }
  } else {
    Serial.printf("[Astro] HTTP %d (negative = connection/timeout error)\n", code);
  }
  http.end();
}
