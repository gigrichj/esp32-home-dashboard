#include <Arduino.h>
#include "secrets.h"
#include "panel_display.h"
#include "version.h"
#include "state_mutex.h"
#include "services/wifi_manager.h"
#include "services/mqtt_service.h"
#include "services/weather_service.h"
#include "services/air_quality_service.h"
#include "services/astro_seeing_service.h"
#include "services/aviation_service.h"
#include "services/iss_service.h"
#include "services/trend_history_service.h"
#include "services/spacex_launch_service.h"
#include "services/imagery_service.h"
#include "services/aurora_service.h"
#include "services/wv_astro_service.h"
#include "screens/screen_manager.h"
#include "debug_log.h"
#include "debug_controls.h"

using namespace PanelDisplay;

static const uint32_t WEATHER_POLL_MS      = 10UL * 60UL * 1000UL;
static const uint32_t SPACEX_POLL_MS       = 2UL * 60UL * 60UL * 1000UL; // every couple hours --
                                                                     // launch schedules don't
                                                                     // change minute-to-minute.
                                                                     // Reduced from 4h so a
                                                                     // failed image/landing
                                                                     // fetch's eventual slow-
                                                                     // cadence retry (see
                                                                     // SPACEX_DETAIL_RETRY_MS
                                                                     // below) doesn't wait as
                                                                     // long once fast retries
                                                                     // are exhausted.
// Bug fix: with a flat 4h interval and lastSpacex starting at 0, the
// first fetch wouldn't happen until 4 REAL HOURS of uptime had passed
// (the "-999 never attempted" sentinel was showing on the SpaceX page
// as a result). Same retry-until-loaded pattern as Weather/Astro/Air
// Quality now applies -- fast retry until first success, then settle
// into the slow cadence. Was 150s; lowered to 115s per follow-up
// feedback for a faster initial retry, while still avoiding the other
// retry timers (60/75/90/105s) that caused the earlier flicker bug.
static const uint32_t SPACEX_RETRY_MS      = 115UL * 1000UL;
// Mission image + booster landing info now retry on their own cadence,
// independent of the list-fetch interval above -- see the dedicated
// block in networkTask() for why. Same 150s value as SPACEX_RETRY_MS,
// kept as a separate constant since the two timers serve different
// purposes and may need to diverge later.
static const uint32_t SPACEX_DETAIL_RETRY_MS = 150UL * 1000UL;
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
// Kp and multi-day astro forecasts don't change fast, so both use a
// single slow interval with no fast-retry escalation -- unlike Weather/
// Air Quality/Astro/Precip, a slow first successful fetch after boot
// isn't costly here (nobody needs an aurora reading or a WV trip-planning
// forecast within the first few minutes of the board powering on).
static const uint32_t AURORA_POLL_MS     = 30UL * 60UL * 1000UL;
static const uint32_t WV_ASTRO_POLL_MS   = 35UL * 60UL * 1000UL; // deliberately offset from ASTRO_POLL_MS/AURORA_POLL_MS
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
  uint32_t lastWeather = 0, lastAviation = 0, lastIss = 0, lastAirQuality = 0, lastAstro = 0, lastSpacex = 0, lastAurora = 0, lastWvAstro = 0;
  uint32_t lastPrecipRetry = 0;
  uint32_t lastImagery = 0;
  static const uint32_t IMAGERY_ROTATE_MS = 15UL * 60UL * 1000UL; // rotate the Imagery page's image every 15 minutes
  bool astroDataLoaded = false;
  bool weatherDataLoaded = false;
  bool spacexDataLoaded = false;
  int spacexRetryCount = 0;
  uint32_t lastSpacexDetail = 0;
  int spacexDetailRetryCount = 0;
  String lastSpacexDetailAttemptId = ""; // tracks which launchId spacexDetailRetryCount currently applies to
  String lastSpacexDetailLaunchId = ""; // tracks which launch's image +
                                         // landing info we last fetched,
                                         // so those extra heavy fetches
                                         // only re-run when "next" changes.
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

    // Imagery page: rotate to a different random embedded image every 15
    // minutes. Local JPEG decode only, no network fetch -- independent of
    // the heavyFetchThisCycle bookkeeping below.
    if (now - lastImagery > IMAGERY_ROTATE_MS) {
      lastImagery = now;
      imagery_update();
    }

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
        g_lastWeatherSuccessMs = now;
      } else if (!weatherDataLoaded) {
        weatherRetryCount++;
      }
      // Precip rides along with this main bundle and is usually already
      // valid by the time it gets here (see comment below on the dedicated
      // fast-retry block) -- that block's g_lastPrecipSuccessMs update
      // only fires while precip *hasn't* loaded yet, so once it succeeds
      // here first, that block never runs again and PRECIP AGE was stuck
      // showing no data forever. Checked independently of g_weather.valid
      // since precip has its own validity flag.
      if (g_precipHourlyValid) {
        g_lastPrecipSuccessMs = now;
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
        g_lastAirQualitySuccessMs = now;
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
        g_lastAstroSuccessMs = now;
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
        g_lastPrecipSuccessMs = now;
      } else {
        precipRetryCount++;
      }
      debug_log("precip retry fetch done");
      heavyFetchThisCycle = true;
    }
    // Restored alongside aviation_service.cpp's fetch functions -- see
    // that file's comment on aviation_service_update() for why.
    if (!heavyFetchThisCycle && now - lastAviation > g_aviationPollMs) {
      lastAviation = now;
      debug_log("aviation fetch start");
      aviation_service_update();
      if (g_aviationStatus.lastHttpCode == 200) {
        g_lastAviationSuccessMs = now;
      }
      debug_log("aviation fetch done");
    }
    if (!heavyFetchThisCycle) {
      aviation_service_detail_loop();
    }
    if (!heavyFetchThisCycle && now - lastIss > ISS_POLL_MS) {
      lastIss = now;
      debug_log("iss fetch start");
      iss_service_update();
      if (g_iss.valid) {
        g_lastIssSuccessMs = now;
      }
      debug_log("iss fetch done");
    }
    if (!heavyFetchThisCycle && now - lastAurora > AURORA_POLL_MS) {
      lastAurora = now;
      debug_log("aurora fetch start");
      aurora_service_update();
      debug_log("aurora fetch done");
      heavyFetchThisCycle = true;
    }
    if (!heavyFetchThisCycle && now - lastWvAstro > WV_ASTRO_POLL_MS) {
      lastWvAstro = now;
      debug_log("wv astro fetch start");
      wv_astro_service_update();
      debug_log("wv astro fetch done");
      heavyFetchThisCycle = true;
    }
    // Not urgent -- deferred to the next cycle like Aviation/ISS rather
    // than forcing itself in alongside a heavy fetch already running.
    // Same MAX_FAST_RETRIES cap as Weather/Air Quality/Precip -- without
    // this, a persistently-timing-out endpoint (like the -11 read
    // timeouts seen on this and other external APIs) would get retried
    // every 150s forever instead of backing off to the slow cadence.
    uint32_t spacexInterval = (spacexDataLoaded || spacexRetryCount >= MAX_FAST_RETRIES)
                                  ? SPACEX_POLL_MS : SPACEX_RETRY_MS;
    if (!heavyFetchThisCycle && now - lastSpacex > spacexInterval) {
      lastSpacex = now;
      debug_log("spacex fetch start");
      spacex_launch_service_update();
      if (g_spacexValid) {
        spacexDataLoaded = true;
        spacexRetryCount = 0;
        g_lastSpacexSuccessMs = now;
      } else if (!spacexDataLoaded) {
        spacexRetryCount++;
      }
      debug_log("spacex fetch done");
    }

    // Mission image + booster landing info retried on their own faster
    // cadence, independent of the list-fetch interval above. Previously
    // both were nested inside that block, so a failed attempt (e.g.
    // during a weak-WiFi window, which produced repeated HTTP -11/-1
    // errors in the field) had to wait for the full SPACEX_POLL_MS
    // list-refresh interval before trying again -- up to hours. Only
    // fires once the list has loaded at least once and only while the
    // current "next" launch's detail hasn't been successfully fetched
    // yet. Backs off to the slow SPACEX_POLL_MS cadence once
    // MAX_FAST_RETRIES is hit, so a persistently broken image URL
    // doesn't retry every 150s forever.
    if (!heavyFetchThisCycle && spacexDataLoaded && g_spacexLaunchCount > 0 &&
        g_spacexLaunches[0].launchId != lastSpacexDetailLaunchId) {
      if (g_spacexLaunches[0].launchId != lastSpacexDetailAttemptId) {
        // New launch to fetch details for -- reset the backoff so it
        // gets fresh fast retries instead of inheriting a previous
        // launch's exhausted retry count.
        lastSpacexDetailAttemptId = g_spacexLaunches[0].launchId;
        spacexDetailRetryCount = 0;
        lastSpacexDetail = 0; // allow an attempt this same cycle
      }
      uint32_t spacexDetailInterval = (spacexDetailRetryCount >= MAX_FAST_RETRIES) ? SPACEX_POLL_MS : SPACEX_DETAIL_RETRY_MS;
      if (now - lastSpacexDetail > spacexDetailInterval) {
        lastSpacexDetail = now;
        debug_log("spacex detail fetch start");
        // Only cache this launchId as "done" if both fetches actually
        // succeeded -- a failure now retries on the next
        // SPACEX_DETAIL_RETRY_MS cycle instead of being silently
        // abandoned until "next" happens to change.
        bool imageOk = spacex_fetch_next_image();
        bool landingOk = spacex_fetch_next_landing_info();
        if (imageOk && landingOk) {
          lastSpacexDetailLaunchId = g_spacexLaunches[0].launchId;
          spacexDetailRetryCount = 0;
          g_lastSpacexDetailSuccessMs = now;
        } else {
          spacexDetailRetryCount++;
        }
        debug_log("spacex detail fetch done");
      }
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
  state_mutex_init(); // must exist before any task that could touch shared state starts
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
  imagery_init(); // one-time local decode, no network/boot-order dependency

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
