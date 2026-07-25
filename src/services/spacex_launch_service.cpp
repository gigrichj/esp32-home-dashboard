#include "spacex_launch_service.h"
#include "wifi_manager.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <time.h>
#include <JPEGDEC.h>
#include "../state_mutex.h"

SpacexLaunch g_spacexLaunches[SPACEX_MAX_LAUNCHES];
int g_spacexLaunchCount = 0;
bool g_spacexValid = false;
int g_spacexLastHttpCode = -999;

uint16_t* g_spacexImagePixels = nullptr;
int g_spacexImageWidth = 0;
int g_spacexImageHeight = 0;
bool g_spacexImageValid = false;

bool g_spacexLandingValid = false;
bool g_spacexLandingAttempt = false;
String g_spacexLandingLocation = "";
String g_spacexLandingAbbrev = "";
String g_spacexLandingType = "";

// Same jpegDrawCallback/s_decodeTarget pattern as aviation_service.cpp's
// fetchAndDecodePhoto() -- file-scoped statics here don't collide with
// that file's own copy, so this is a straightforward duplication rather
// than a shared/refactored helper.
static uint16_t* s_decodeTarget = nullptr;
static int s_decodeTargetW = 0;
static int s_decodeTargetH = 0;

static int jpegDrawCallback(JPEGDRAW *pDraw) {
  if (s_decodeTarget == nullptr) return 0;
  for (int row = 0; row < pDraw->iHeight; row++) {
    int destY = pDraw->y + row;
    if (destY < 0 || destY >= s_decodeTargetH) continue;
    uint16_t *destRow = s_decodeTarget + (size_t)destY * s_decodeTargetW;
    const uint16_t *srcRow = pDraw->pPixels + (size_t)row * pDraw->iWidth;
    for (int col = 0; col < pDraw->iWidth; col++) {
      int destX = pDraw->x + col;
      if (destX < 0 || destX >= s_decodeTargetW) continue;
      destRow[destX] = srcRow[col];
    }
  }
  return 1;
}

// Same yield-safe manual read loop used throughout this project -- a
// plain http.getString() risks the same FreeRTOS watchdog crash already
// fixed elsewhere (aviation, air quality, weather).
static bool readHttpBodySafely(HTTPClient& http, String& outPayload, const char* sourceName) {
  int payloadLen = http.getSize();
  int bufSize = (payloadLen > 0) ? payloadLen + 1 : 65536;
  char *rawBuf = (char *)heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
  if (rawBuf == nullptr) {
    Serial.printf("[SpaceX] %s payload buffer alloc failed\n", sourceName);
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t readTotal = 0;
  uint32_t startMs = millis();
  bool readError = false;
  while (readTotal < (size_t)(bufSize - 1) && millis() - startMs < 15000) {
    if (!http.connected() && stream->available() == 0) break;
    size_t avail = stream->available();
    if (avail > 0) {
      int toRead = (int)min(avail, (size_t)(bufSize - 1 - readTotal));
      int r = stream->readBytes(rawBuf + readTotal, toRead);
      if (r <= 0) { readError = true; break; }
      readTotal += r;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5)); // yield -- the critical fix
    }
  }
  rawBuf[readTotal] = '\0';

  if (readError) {
    Serial.printf("[SpaceX] %s payload read error\n", sourceName);
    free(rawBuf);
    return false;
  }

  outPayload = String(rawBuf);
  free(rawBuf);
  return true;
}

// Same "days from civil" UTC conversion duplicated per-file elsewhere in
// this project (this toolchain has no timegm(), and mktime() assumes
// local time). Launch times need minute precision (unlike the
// hourly-only weather data this pattern was originally used for), so
// minutes are folded in directly rather than added as a separate step.
static uint32_t utcToUnix(int year, int month, int day, int hour, int minute) {
  int y = year;
  int m = month;
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  int yoe = (int)(y - era * 400);
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long daysSinceEpoch = era * 146097L + doe - 719468L;
  return (uint32_t)(daysSinceEpoch * 86400L + (long)hour * 3600L + (long)minute * 60L);
}

void spacex_launch_service_update() {
  if (!wifi_manager_is_connected()) return;

  HTTPClient http;
  const char* url = "https://ll.thespacedevs.com/2.0.0/launch/upcoming/?lsp__name=SpaceX&limit=15";
  http.begin(url);
  // Same setTimeout fix as astro_seeing_service.cpp's 7Timer fetch --
  // the default HTTPClient timeout (~5s) was apparently too short for
  // this host on this network, showing up as constant HTTP -11
  // (read timeout) with the SpaceX page stuck on "No launch data yet".
  http.setTimeout(15000);
  // Some public APIs slow-walk or drop requests with no User-Agent at
  // all (browsers always send a real one, which is why it worked fine
  // in a browser test but timed out repeatedly from this device).
  http.setUserAgent("esp32-home-dashboard/1.0");
  // Same useHTTP10 fix as every other manual-read-loop JSON fetch in this
  // project -- without it, a chunked-transfer-encoding response corrupts
  // the raw stream read.
  http.useHTTP10(true);
  int code = http.GET();
  state_lock();
  g_spacexLastHttpCode = code;
  state_unlock();
  if (code != 200) {
    Serial.printf("[SpaceX] HTTP %d\n", code);
    http.end();
    return;
  }

  String payload;
  if (!readHttpBodySafely(http, payload, "SpaceX")) {
    http.end();
    return;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[SpaceX] JSON parse error: %s\n", err.c_str());
    return;
  }

  JsonArray results = doc["results"].as<JsonArray>();
  time_t now = time(nullptr);
  uint32_t cutoffUnix = (now > 100000) ? (uint32_t)now + (30UL * 86400UL) : 0xFFFFFFFF;
  // LL2's "upcoming" endpoint keeps a launch listed for a while after its
  // NET (liftoff) time passes -- that's actually needed, since it's the
  // only way "In Flight"/"Success" status ever becomes visible (the NET
  // is always in the past once a rocket has actually launched). But
  // without any lower bound, a launch just stays in the list forever as
  // long as the API keeps returning it, showing up as stale days later.
  // 12 hours is enough to see status settle from Go -> In Flight ->
  // Success/Failure, short enough that a launch won't linger past when
  // it's actually relevant.
  static const uint32_t PAST_LAUNCH_GRACE_SEC = 12UL * 3600UL;
  uint32_t graceFloorUnix = (now > 100000 && (uint32_t)now > PAST_LAUNCH_GRACE_SEC)
                                ? (uint32_t)now - PAST_LAUNCH_GRACE_SEC : 0;

  // Locked for the whole loop (plus the final validity flag) --
  // g_spacexLaunchCount and g_spacexLaunches[] must never be visible to a
  // reader in a state where the count implies more entries than have
  // actually been written yet.
  state_lock();
  g_spacexLaunchCount = 0;
  for (JsonObject launch : results) {
    if (g_spacexLaunchCount >= SPACEX_MAX_LAUNCHES) break;

    String netStr = launch["net"] | "";
    int yr, mo, dy, hr, mi;
    uint32_t netUnix = 0;
    if (sscanf(netStr.c_str(), "%d-%d-%dT%d:%d", &yr, &mo, &dy, &hr, &mi) == 5) {
      netUnix = utcToUnix(yr, mo, dy, hr, mi);
    }

    // Skip anything beyond 30 days out, or more than the grace window in
    // the past -- the API returns launches in ascending date order
    // already, so the upper bound naturally trims the tail, but past
    // launches need this explicit lower-bound check since they'd
    // otherwise never get filtered out at all.
    if (netUnix == 0 || netUnix > cutoffUnix || netUnix < graceFloorUnix) continue;

    SpacexLaunch& out = g_spacexLaunches[g_spacexLaunchCount];
    out.displayName = launch["name"] | "";
    out.rocketName = launch["rocket"]["configuration"]["name"] | "";
    out.missionName = launch["mission"]["name"] | "";
    out.padName = launch["pad"]["name"] | "";
    out.locationName = launch["pad"]["location"]["name"] | "";
    out.statusName = launch["status"]["name"] | "";
    out.imageUrl = launch["image"] | "";
    out.launchId = launch["id"] | "";
    out.netUnix = netUnix;
    g_spacexLaunchCount++;
  }
  g_spacexValid = true;
  state_unlock();
}

// Mirrors aviation_service.cpp's fetchAndDecodePhoto() pattern -- fetches
// raw JPEG bytes directly into a PSRAM buffer (not through readHttpBodySafely,
// since that returns a null-terminated String, not appropriate for binary
// JPEG data) and decodes via JPEGDEC into an RGB565 pixel buffer.
void spacex_fetch_next_image() {
  if (!wifi_manager_is_connected()) return;
  if (g_spacexLaunchCount == 0) return;
  String url = g_spacexLaunches[0].imageUrl;
  if (url.length() == 0) return;

  HTTPClient http;
  http.begin(url);
  http.setTimeout(15000); // same fix as the list/landing fetches above
  http.setUserAgent("esp32-home-dashboard/1.0");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[SpaceX] image fetch HTTP %d\n", code);
    http.end();
    return;
  }

  // Raised from 250000 -- real LL2 mission photos have been coming back
  // around 660KB, well over the old cap, so the image never once passed
  // this check. 900000 gives headroom above that while still rejecting
  // anything wildly oversized/malformed.
  int len = http.getSize();
  if (len <= 0 || len > 900000) {
    Serial.printf("[SpaceX] image size invalid: %d\n", len);
    http.end();
    return;
  }

  uint8_t *jpegBuf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (jpegBuf == nullptr) {
    Serial.println("[SpaceX] image buffer alloc failed");
    http.end();
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t readTotal = 0;
  uint32_t startMs = millis();
  bool readError = false;
  while (http.connected() && readTotal < (size_t)len && millis() - startMs < 15000) {
    size_t avail = stream->available();
    if (avail > 0) {
      int toRead = (int)min((size_t)avail, (size_t)len - readTotal);
      int r = stream->readBytes(jpegBuf + readTotal, toRead);
      if (r <= 0) { readError = true; break; }
      readTotal += r;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5)); // yield -- same critical fix used throughout this project
    }
  }
  http.end();

  if (readError || readTotal == 0) {
    Serial.println("[SpaceX] image payload read error");
    free(jpegBuf);
    return;
  }

  JPEGDEC jpeg;
  if (jpeg.openRAM(jpegBuf, (int)readTotal, jpegDrawCallback)) {
    int w = jpeg.getWidth();
    int h = jpeg.getHeight();
    Serial.printf("[SpaceX] JPEG source dimensions %dx%d\n", w, h);

    // Two-stage shrink. Stage 1: decode at 1/8 scale (JPEGDEC's largest
    // built-in reduction) into a temporary buffer -- for a 4096-wide
    // source that's still 512px, too big to fit the ~220px-wide area
    // reserved on the SpaceX page. Stage 2: manually downsample that
    // into a small buffer sized to actually fit on screen, so the whole
    // photo is visible (just small) instead of the screen only showing
    // a cropped corner of an oversized image.
    int decodedW = w / 8;
    int decodedH = h / 8;

    uint16_t *decodeBuf = (uint16_t *)heap_caps_malloc((size_t)decodedW * decodedH * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (decodeBuf != nullptr) {
      s_decodeTarget = decodeBuf;
      s_decodeTargetW = decodedW;
      s_decodeTargetH = decodedH;
      jpeg.decode(0, 0, JPEG_SCALE_EIGHTH);
      s_decodeTarget = nullptr;

      // Target box reserved on the SpaceX page (see draw_spacex() in
      // screen_manager.cpp) -- top-right, beside the Starship/Super
      // Heavy badge and above the divider line further down. Grown to
      // 1.2in tall (96px, this project's established 80px/in scale) per
      // follow-up feedback -- paired with moving the image up further
      // (see draw_spacex()) so it still clears the divider line at this
      // larger size. Width derived from the real source aspect ratio
      // rather than assumed, in case future images come in a different
      // shape.
      int targetH = 96;
      int targetW = (int)((float)targetH * w / h);
      if (targetW < 1) targetW = 1;

      uint16_t *finalBuf = (uint16_t *)heap_caps_malloc((size_t)targetW * targetH * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
      if (finalBuf != nullptr) {
        // Simple nearest-neighbor downsample -- plenty good for a small
        // thumbnail on this display, and cheap enough to run on every
        // fetch without pulling in a real resampling library.
        for (int dy = 0; dy < targetH; dy++) {
          int srcY = dy * decodedH / targetH;
          for (int dx = 0; dx < targetW; dx++) {
            int srcX = dx * decodedW / targetW;
            finalBuf[dy * targetW + dx] = decodeBuf[srcY * decodedW + srcX];
          }
        }

        // Locked as one atomic swap -- this frees the old pixel buffer
        // and reassigns the pointer. If the UI task's drawRGBBitmap()
        // read this pointer mid-swap, it would dereference freed memory
        // (a real use-after-free, not just a torn read), so this is the
        // single most important lock in this file.
        state_lock();
        if (g_spacexImagePixels != nullptr) {
          free(g_spacexImagePixels);
          g_spacexImagePixels = nullptr;
          g_spacexImageValid = false;
        }
        g_spacexImagePixels = finalBuf;
        g_spacexImageWidth = targetW;
        g_spacexImageHeight = targetH;
        g_spacexImageValid = true;
        state_unlock();
        Serial.printf("[SpaceX] image decoded %dx%d, downsampled to %dx%d\n", decodedW, decodedH, targetW, targetH);
      } else {
        Serial.printf("[SpaceX] final image buffer alloc failed (%dx%d requested)\n", targetW, targetH);
      }

      free(decodeBuf);
    } else {
      Serial.printf("[SpaceX] intermediate decode buffer alloc failed (%dx%d requested)\n", decodedW, decodedH);
    }
    jpeg.close();
  } else {
    Serial.println("[SpaceX] JPEG openRAM failed");
  }

  free(jpegBuf);
}

// Booster landing info for the next launch -- only available via LL2's
// single-launch detail endpoint (not the list/upcoming endpoint used
// above), which returns a much larger response (full agency/rocket/program
// descriptions we don't need, alongside the landing info we do). Reuses
// readHttpBodySafely() since this is JSON text, not binary like the image.
void spacex_fetch_next_landing_info() {
  if (!wifi_manager_is_connected()) return;
  if (g_spacexLaunchCount == 0) return;
  String launchId = g_spacexLaunches[0].launchId;
  if (launchId.length() == 0) return;

  HTTPClient http;
  String url = "https://ll.thespacedevs.com/2.0.0/launch/" + launchId + "/";
  http.begin(url);
  http.setTimeout(15000); // same fix as the list fetch above
  http.setUserAgent("esp32-home-dashboard/1.0");
  http.useHTTP10(true);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[SpaceX] landing detail HTTP %d\n", code);
    http.end();
    return;
  }

  String payload;
  if (!readHttpBodySafely(http, payload, "SpaceX landing detail")) {
    http.end();
    return;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[SpaceX] landing detail JSON parse error: %s\n", err.c_str());
    return;
  }

  // rocket.launcher_stage is an array; landing info (if any) lives on the
  // first stage's entry. Absent entirely for launches with no landing
  // attempt planned (e.g. expendable missions), hence the size() check.
  JsonArray stages = doc["rocket"]["launcher_stage"].as<JsonArray>();
  if (stages.size() == 0) {
    state_lock();
    g_spacexLandingAttempt = false;
    g_spacexLandingValid = true;
    state_unlock();
    return;
  }

  JsonObject landing = stages[0]["landing"];
  state_lock();
  g_spacexLandingAttempt = landing["attempt"] | false;
  g_spacexLandingLocation = landing["location"]["name"] | "";
  g_spacexLandingAbbrev = landing["location"]["abbrev"] | "";
  g_spacexLandingType = landing["type"]["abbrev"] | "";
  g_spacexLandingValid = true;
  state_unlock();
}

