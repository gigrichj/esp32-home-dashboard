#include <Arduino.h>
#include "secrets.h"
#include "panel_display.h"
#include "version.h"
#include "services/wifi_manager.h"
#include "services/mqtt_service.h"
#include "services/weather_service.h"
#include "services/air_quality_service.h"
#include "services/astro_seeing_service.h"
#include "services/aviation_service.h"
#include "services/iss_service.h"
#include "services/trend_history_service.h"
#include "screens/screen_manager.h"
#include "debug_log.h"
#include "debug_controls.h"

using namespace PanelDisplay;

static const uint32_t WEATHER_POLL_MS      = 10UL * 60UL * 1000UL;
static const uint32_t AIR_QUALITY_POLL_MS  = 25UL * 60UL * 1000UL; // deliberately not a clean
                                                                     // multiple of WEATHER_POLL_MS,
                                                                     // so the two heavy HTTPS fetches
                                                                     // rarely land in the same
                                                                     // networkTask iteration.
static const uint32_t ASTRO_POLL_MS        = 30UL * 60UL * 1000UL; // seeing forecasts change slowly;
                                                                     // also deliberately offset from
                                                                     // the other poll intervals.
static const uint32_t ASTRO_RETRY_MS       = 60UL * 1000UL; // 60s retry cadence
                                                                     // until the first successful fetch,
                                                                     // then settles to ASTRO_POLL_MS --
                                                                     // same pattern used for ISS crew count.

// Same "retry fast until first success, then settle into the normal
// cadence" pattern extended to Weather and Air Quality (previously only
// Astro/ISS crew count/TLE had it) -- these two were retrying at their
// full slow interval even after a failed fetch, so a transient miss on
// first boot could take a full 10-25 minutes to recover from instead of
// about a minute.
//
// IMPORTANT: these all started at the same 60s value as the pre-existing
// ASTRO_RETRY_MS above, which meant that right after boot -- before any
// of them had succeeded -- all 4 fast-retry timers (astro, weather, air
// quality, precip) became "due" at roughly the same wall-clock moment
// and kept re-aligning every 60s after that, stacking multiple heavy
// HTTPS fetches into the same loop iteration. That's the exact pattern
// that caused the original flicker bug, just now happening reliably
// every ~60s during early boot instead of by rare coincidence. Staggered
// here the same way the slow POLL_MS intervals are deliberately offset.
static const uint32_t WEATHER_RETRY_MS      = 75UL * 1000UL;
static const uint32_t AIR_QUALITY_RETRY_MS  = 90UL * 1000UL;
// The 24hr precip forecast is fetched as part of weather_service_update(),
// but given its own independent retry schedule here rather than forcing
// the other 3 weather fetches (current conditions/forecast/UV) to also
// re-run more often -- retrying all 4 stacked HTTPS calls on a fast
// cadence risks the same flicker issue already fixed by spacing them out.
static const uint32_t PRECIP_RETRY_MS       = 105UL * 1000UL;

// Caps the fast retry above -- after this many failed fast-cadence
// attempts (~5 minutes at 60s each), fall back to the normal slow
// interval even if still not loaded. A persistently-failing endpoint
// (e.g. a TLS-handshake timeout, HTTP code -11) was being retried every
// 60s indefinitely, and repeated failed-handshake attempts are suspected
// to leak heap on this platform -- consistent with a crash observed
// after about 7 minutes of uptime once this fast-retry pattern was
// added, with Air Quality's endpoint timing out on every attempt.
static const int MAX_FAST_RETRIES = 5;

static const uint32_t ISS_POLL_MS        = 60UL * 1000UL;
static const uint32_t DRAW_INTERVAL_MS   = 200UL;

bool wasInSetupMode = false;
bool setupModeActive = false;

static void draw_setup_screen() {
  uint16_t bg = screen.color565(10, 12, 16);
  uint16_t text = screen.color565(235, 240, 245);
  uint16_t accent = screen.color565(70, 130, 220);

  screen.fillScreen(bg);
  screen.setTextSize(3);
  screen.setTextColor(accent, bg);
  screen.setTextDatum(textdatum_t::top_left);
  screen.drawString("WIFI SETUP NEEDED", 30, 60);

  screen.setTextSize(2);
  screen.setTextColor(text, bg);
  screen.drawString("1. On your phone, connect to WiFi network:", 30, 140);
  screen.setTextColor(accent, bg);
  screen.drawString("   ESP32-Dashboard-Setup", 30, 175);

  screen.setTextColor(text, bg);
  screen.drawString("2. Open a browser and go to:", 30, 230);
  screen.setTextColor(accent, bg);
  screen.drawString("   http://192.168.4.1", 30, 265);

  screen.setTextColor(text, bg);
  screen.drawString("3. Enter your home WiFi name and password.", 30, 320);
  screen.drawString("   The device will restart and connect.", 30, 350);

  String lastSsid = wifi_manager_last_attempted_ssid();
  if (lastSsid.length() > 0) {
    screen.setTextSize(1);
    screen.setTextColor(screen.color565(200, 90, 90), bg);
    char line[96];
    snprintf(line, sizeof(line), "Last attempt: '%s' -> status code %d",
             lastSsid.c_str(), wifi_manager_last_status_code());
    screen.drawString(line, 30, 420);
    screen.drawString("(3=connected, 1=no network found, 4=connect failed/bad password, 6=disconnected)", 30, 440);
  }

  screen.setTextSize(1);
  screen.setTextColor(screen.color565(90, 100, 110), bg);
  screen.setTextDatum(textdatum_t::top_right);
  screen.drawString(FIRMWARE_VERSION, WIDTH - 6, HEIGHT - 14);
}

// Touch is polled far more often than the screen redraws (every ~20ms vs
// every ~200ms). This matters for quick gestures like double-tap: with
// touch only sampled once per draw cycle, a real double-tap's second tap
// -- and the brief finger-up moment between taps -- could land entirely
// within a single 200ms gap and never get seen at all, so the gesture
// handler only ever saw one long continuous touch. Decoupling the two
// keeps the display's redraw workload (and DMA/flicker behavior) exactly
// as before, while letting quick multi-tap gestures actually register.
static const uint32_t TOUCH_POLL_MS = 20UL;

void uiTask(void* param) {
  uint32_t msSinceLastDraw = 0;

  for (;;) {
    if (setupModeActive) {
      draw_setup_screen();
      screen.present();
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    uint16_t touchX = 0, touchY = 0;
    bool touched = screen.readTouch(&touchX, &touchY);
    screen_manager_handle_touch(touched, touchX, touchY);

    msSinceLastDraw += TOUCH_POLL_MS;
    if (msSinceLastDraw >= DRAW_INTERVAL_MS) {
      msSinceLastDraw = 0;

      screen_manager_draw();

      if (!screen.present()) {
        Serial.println("[uiTask] present failed; restarting");
        Serial.flush();
        delay(100);
        ESP.restart();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
  }
}

void networkTask(void* param) {
  uint32_t lastWeather = 0, lastAviation = 0, lastIss = 0, lastAirQuality = 0, lastAstro = 0;
  uint32_t lastPrecipRetry = 0;
  bool astroDataLoaded = false;
  bool weatherDataLoaded = false;
  bool airQualityDataLoaded = false;
  int weatherRetryCount = 0;
  int airQualityRetryCount = 0;
  int precipRetryCount = 0;

  for (;;) {
    wifi_manager_loop();
    debug_controls_update_min_heap();
    setupModeActive = wifi_manager_in_setup_mode();

    if (setupModeActive) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (wasInSetupMode) {
      wasInSetupMode = false;
      mqtt_service_begin();
    }

    uint32_t now = millis();

    // Set whenever a heavy Weather/Air Quality/Astro fetch runs this cycle,
    // so Aviation and ISS can defer to the next cycle instead of firing
    // back-to-back with one of those fetches -- landing two heavy HTTPS/JSON
    // operations in the same cycle was starving the ESP32 of heap/TLS buffer
    // space, causing Aviation's JSON parse to fail (or the whole board to
    // reboot) whenever its poll happened to coincide with Astro's.
    bool heavyFetchThisCycle = false;

    uint32_t weatherInterval = (weatherDataLoaded || weatherRetryCount >= MAX_FAST_RETRIES) ? WEATHER_POLL_MS : WEATHER_RETRY_MS;
    if (now - lastWeather > weatherInterval) {
      lastWeather = now;
      debug_log("weather fetch start");
      weather_service_update();
      if (g_weather.valid) {
        weatherDataLoaded = true;
        weatherRetryCount = 0;
      } else if (!weatherDataLoaded) {
        weatherRetryCount++;
      }
      debug_log("weather fetch done");
      heavyFetchThisCycle = true;
    }
    // Air quality gets its own, longer, deliberately-offset poll interval
    // rather than piggybacking on the weather fetch -- doing all 3 HTTPS
    // calls back-to-back was heavy enough on PSRAM/TLS to disrupt the RGB
    // panel's DMA timing and cause flicker (confirmed by isolation test).
    uint32_t airQualityInterval = (airQualityDataLoaded || airQualityRetryCount >= MAX_FAST_RETRIES) ? AIR_QUALITY_POLL_MS : AIR_QUALITY_RETRY_MS;
    if (now - lastAirQuality > airQualityInterval) {
      lastAirQuality = now;
      debug_log("air quality fetch start");
      air_quality_service_update();
      if (g_airQuality.valid) {
        airQualityDataLoaded = true;
        airQualityRetryCount = 0;
      } else if (!airQualityDataLoaded) {
        airQualityRetryCount++;
      }
      debug_log("air quality fetch done");
      vTaskDelay(pdMS_TO_TICKS(200)); // let the display catch its breath
      heavyFetchThisCycle = true;
    }
    uint32_t astroInterval = astroDataLoaded ? ASTRO_POLL_MS : ASTRO_RETRY_MS;
    if (now - lastAstro > astroInterval) {
      lastAstro = now;
      debug_log("astro fetch start");
      astro_seeing_service_update();
      if (g_astroLastHttpCode == 200 && g_astroForecastCount > 0) {
        astroDataLoaded = true;
      }
      debug_log("astro fetch done");
      vTaskDelay(pdMS_TO_TICKS(200)); // let the display catch its breath
      heavyFetchThisCycle = true;
    }
    // The 24hr precip forecast rides along with the main weather bundle
    // above once it's loaded, but gets its own independent fast retry
    // here if it hasn't succeeded yet -- a single extra lightweight fetch
    // is safe to retry often, unlike re-running the full 4-call bundle.
    if (!g_precipHourlyValid && !heavyFetchThisCycle && precipRetryCount < MAX_FAST_RETRIES &&
        now - lastPrecipRetry > PRECIP_RETRY_MS) {
      lastPrecipRetry = now;
      debug_log("precip retry fetch start");
      weather_service_update_precip_only();
      if (g_precipHourlyValid) {
        precipRetryCount = 0;
      } else {
        precipRetryCount++;
      }
      debug_log("precip retry fetch done");
      heavyFetchThisCycle = true;
    }
    if (!heavyFetchThisCycle && now - lastAviation > g_aviationPollMs) {
      lastAviation = now;
      debug_log("aviation fetch start");
      aviation_service_update();
      debug_log("aviation fetch done");
    }
    if (!heavyFetchThisCycle) {
      aviation_service_detail_loop();
    }
    if (!heavyFetchThisCycle && now - lastIss > ISS_POLL_MS) {
      lastIss = now;
      debug_log("iss fetch start");
      iss_service_update();
      debug_log("iss fetch done");
    }

    // Cheap no-op most iterations -- internally gated to a 5-minute
    // interval, so this doesn't need heavyFetchThisCycle coordination.
    trend_history_update();

    // Moved here (was unconditional, right at the top of the loop) and
    // gated by !heavyFetchThisCycle -- mqtt_service_loop() was calling
    // mqttClient.connect() every time its 5s internal retry interval
    // elapsed, completely uncoordinated with every other heavy operation
    // in this loop. Serial logs showed it failing with "Connection reset
    // by peer" on a steady ~5s beat for the entire session, meaning it
    // was very likely colliding with Weather/Astro/Aviation/ISS fetches
    // repeatedly -- a plausible source of the recurring display glitch,
    // by the same "two heavy network operations landing in one cycle"
    // mechanism already documented for the other services above.
    if (!heavyFetchThisCycle) {
      mqtt_service_loop();
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  debug_controls_record_reset_reason(); // capture ASAP, before anything else can reset the board again
  uint32_t serialStart = millis();
  while (!Serial && millis() - serialStart < 3000) {
    delay(20);
  }

  Serial.println("[boot] display begin");
  if (!screen.begin()) {
    Serial.println("[boot] display FAILED — halting");
    while (true) delay(1000);
  }
  Serial.println("[boot] display ready");

  screen_manager_init();

  wifi_manager_begin();
  setupModeActive = wifi_manager_in_setup_mode();

  xTaskCreatePinnedToCore(uiTask, "uiTask", 8192, nullptr, 1, nullptr, 1);

  if (!setupModeActive) {
    mqtt_service_begin();
    configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
    weather_service_update();
    delay(150);
    air_quality_service_update();
    delay(150);
    astro_seeing_service_update();
    delay(150);
    aviation_service_update();
    delay(150);
    iss_service_update();
  } else {
    wasInSetupMode = true;
  }

  xTaskCreatePinnedToCore(networkTask, "networkTask", 24576, nullptr, 1, nullptr, 0); // bumped for JPEG decode headroom
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
