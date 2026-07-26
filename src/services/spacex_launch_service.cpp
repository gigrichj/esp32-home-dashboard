#include "spacex_launch_service.h"
#include "wifi_manager.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <time.h>
#include <JPEGDEC.h>
#include <PNGdec.h>
#include <new>
#include "../state_mutex.h"

// ============================================================================
// PNG MISSION IMAGE: ROOT CAUSE HISTORY (for future reference)
// ============================================================================
// LL2 mission images aren't always JPEG -- some launches (this one included)
// serve a PNG instead. Adding PNG support (below) surfaced two separate,
// serious bugs, both now fixed:
//
// BUG 1 -- Heap corruption crash (reboot every time on a real PNG):
//   Root cause: PNGdec's own getLineAsRGB565() function has a bug specific
//   to its alpha-blending code path, only exercised by truecolor+alpha PNGs
//   (color type 6) -- confirmed via an IHDR dump that the real mission image
//   is color type 6, while a generic test PNG (no alpha) decoded perfectly
//   every time. Since PlatformIO re-fetches this vendored library fresh on
//   every build, we can't patch it directly -- instead, pngDrawCallback()
//   bypasses getLineAsRGB565() entirely and does its own manual RGB565
//   conversion straight from the raw decoded pixels (pDraw->pPixels),
//   handling both RGB and RGBA source formats.
//   Diagnostic note: the ON-DEVICE exception decoder (pio device monitor's
//   esp32_exception_decoder) gave WRONG function/line attribution for every
//   single crash this bug caused, because our workflow flashes a merged
//   binary built on GitHub Actions via esptool.py directly -- never a local
//   `pio run` -- so the local .pio/build/.../firmware.elf used for on-device
//   symbolication was stale/mismatched. Resolving the same backtrace
//   addresses with `addr2line -e <the actual uploaded firmware.elf> -f -C -i
//   <address>` gave correct, trustworthy attribution and was what actually
//   cracked this. If a crash's on-device backtrace ever looks like it's
//   pointing at unrelated code, re-resolve it against the real ELF before
//   trusting it.
//
// BUG 2 -- Persistent display flicker on every page, first triggered the
//   moment a real PNG successfully decoded and displayed:
//   Root cause: unlike the JPEG path (JPEGDEC's built-in 1/8-scale decode
//   only ever processes ~90 rows), PNGdec has no equivalent and decodes
//   every row of the image at full resolution (~722 rows here) before we
//   get to shrink it. That's a much longer sustained burst of PSRAM traffic
//   than any JPEG this project has drawn -- and the RGB LCD's DMA refill
//   also lives on the PSRAM bus, so this is the same root-cause class as
//   this project's original display-flicker bug (see platformio.ini's
//   bounce-buffer history). Fixed with a periodic vTaskDelay() yield inside
//   pngDrawCallback() (see below) so the display DMA gets regular breathing
//   room during the decode instead of PSRAM being monopolized in one
//   uninterrupted burst.
// ============================================================================

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

// PNG counterpart to the statics/callback above. PNGdec has no built-in
// reduced-scale decode like JPEGDEC's JPEG_SCALE_EIGHTH, so instead of a
// two-stage shrink, downsampling happens directly in the line callback:
// for each full-resolution row PNGdec hands us, check whether it's one of
// the rows our small target image actually needs (nearest-neighbor row
// mapping), and only if so, convert that one row to RGB565 and sample it
// into the final small buffer. Avoids ever allocating a full-resolution
// intermediate buffer, which matters since PNG mission photos have been
// seen at several thousand pixels wide.
static uint16_t* s_pngFinalBuf = nullptr;
static int s_pngFinalW = 0;
static int s_pngFinalH = 0;
static int s_pngSrcW = 0;
static int s_pngSrcH = 0;
static uint16_t* s_pngLineBuf = nullptr;
static PNG* s_pngObj = nullptr;

static int pngDrawCallback(PNGDRAW *pDraw) {
  if (s_pngFinalBuf == nullptr || s_pngLineBuf == nullptr || s_pngObj == nullptr) return 0;

  // Yield periodically through the full-resolution decode -- unlike the
  // JPEG path (JPEGDEC's built-in 1/8-scale decode only ever processes
  // ~90 rows for a typical mission photo), PNGdec has no equivalent and
  // decodes every one of this image's ~722 rows at full width before we
  // get to shrink it down. That's a much longer sustained burst of PSRAM
  // traffic (inflate + de-filter + our RGB565 conversion) than any JPEG
  // this project has ever drawn, and was observed to trigger persistent
  // display flicker across every page the first time a real PNG decoded
  // successfully -- the RGB LCD's DMA refill also lives on the PSRAM
  // bus, the same root-cause class as this project's original flicker
  // bug. Yielding briefly every few rows gives the display DMA regular
  // breathing room instead of PSRAM being monopolized for the whole
  // decode in one uninterrupted burst. Runs for every row regardless of
  // whether this particular row is one we actually downsample (below),
  // since PNGdec still does real inflate/de-filter work on skipped rows.
  if ((pDraw->y % 20) == 0) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  int srcY = pDraw->y;

  bool needed = false;
  for (int ty = 0; ty < s_pngFinalH; ty++) {
    if ((ty * s_pngSrcH) / s_pngFinalH == srcY) { needed = true; break; }
  }
  if (!needed) return 1; // this source row isn't sampled by our downsample, skip the RGB565 conversion

  // Manual RGB565 conversion, bypassing PNGdec's own getLineAsRGB565() --
  // the real launch photo (truecolor+alpha, PNG color type 6) crashed
  // with a heap corruption 100% of the time, traced via extensive
  // bracket-checking to somewhere in this decode step, while a same-size
  // plain-RGB test photo (no alpha) decoded and cleaned up completely
  // successfully every time. That strongly points to a bug specific to
  // getLineAsRGB565()'s alpha-blending code path (only exercised when
  // the source actually has alpha), which we can't patch directly since
  // PlatformIO re-fetches this vendored library fresh on every build.
  // Converting manually from the raw native pixels avoids that code path
  // entirely. Only handles the two pixel types actually seen in practice
  // (8-bit RGB and RGBA); anything else is skipped rather than risking a
  // misread of an unfamiliar layout.
  int bytesPerPixel = (pDraw->iPixelType == 6) ? 4 : (pDraw->iPixelType == 2) ? 3 : 0;
  if (bytesPerPixel == 0) {
    Serial.printf("[SpaceX] PNG pixel type %d not handled by manual RGB565 conversion, skipping line\n", pDraw->iPixelType);
    return 1;
  }
  int lineWidth = pDraw->iWidth;
  if (lineWidth > s_pngSrcW) lineWidth = s_pngSrcW; // safety clamp against s_pngLineBuf's allocated size
  for (int x = 0; x < lineWidth; x++) {
    uint8_t *px = pDraw->pPixels + (size_t)x * bytesPerPixel;
    uint16_t r = px[0], g = px[1], b = px[2];
    s_pngLineBuf[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }

  for (int ty = 0; ty < s_pngFinalH; ty++) {
    if ((ty * s_pngSrcH) / s_pngFinalH == srcY) {
      uint16_t *destRow = s_pngFinalBuf + (size_t)ty * s_pngFinalW;
      for (int tx = 0; tx < s_pngFinalW; tx++) {
        int srcX = tx * s_pngSrcW / s_pngFinalW;
        destRow[tx] = s_pngLineBuf[srcX];
      }
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
// Returns true only if the image was fully fetched and decoded --
// main.cpp uses this (along with spacex_fetch_next_landing_info()'s own
// return value) to decide whether to mark this launch's detail fetch as
// "done" or retry it on a later cycle. Previously this returned void and
// main.cpp always marked it done regardless of outcome, so a single
// failed attempt (timeout, oversized image, decode failure, etc.) meant
// the image silently stayed blank until "next" happened to change to a
// different launch -- which could be days away.
// Extracted from the original spacex_fetch_next_image() body, unchanged
// apart from taking the raw buffer/length as parameters -- same two-stage
// shrink (1/8 scale then manual nearest-neighbor downsample to the
// on-screen target box).
static bool decodeAndStoreJpeg(uint8_t *buf, size_t bufLen) {
  bool success = false;

  JPEGDEC jpeg;
  if (jpeg.openRAM(buf, (int)bufLen, jpegDrawCallback)) {
    int w = jpeg.getWidth();
    int h = jpeg.getHeight();
    Serial.printf("[SpaceX] JPEG source dimensions %dx%d\n", w, h);

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
      // 1.25in tall (100px, this project's established 80px/in scale)
      // per follow-up feedback. Width derived from the real source
      // aspect ratio rather than assumed, in case future images come in
      // a different shape.
      int targetH = 100;
      int targetW = (int)((float)targetH * w / h);
      if (targetW < 1) targetW = 1;

      uint16_t *finalBuf = (uint16_t *)heap_caps_malloc((size_t)targetW * targetH * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
      if (finalBuf != nullptr) {
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
        Serial.printf("[SpaceX] JPEG decoded %dx%d, downsampled to %dx%d\n", decodedW, decodedH, targetW, targetH);
        success = true;
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

  return success;
}

// PNG counterpart to decodeAndStoreJpeg() above. See pngDrawCallback()
// near the top of this file for how the direct-to-target downsample
// works without a full-resolution intermediate buffer.
static bool decodeAndStorePng(uint8_t *buf, size_t bufLen) {
  bool success = false;

  // The PNG decoder is allocated on PSRAM rather than declared as a
  // local (stack) variable. PNGdec's internal struct embeds a fixed-size
  // double-line buffer sized by PNG_MAX_BUFFERED_PIXELS (see
  // platformio.ini) -- at our chosen width that buffer alone is roughly
  // 32KB. A local "PNG png;" here put all of that on this task's stack
  // the moment the object was constructed, regardless of the actual
  // image's dimensions, which caused an immediate "Stack canary
  // watchpoint triggered" crash (with a garbled backtrace from the
  // corrupted stack) the first time this ran against a real photo.
  void *pngMem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM);
  if (pngMem == nullptr) {
    Serial.println("[SpaceX] PNG decoder alloc failed");
    return false;
  }
  PNG *png = new (pngMem) PNG();

  if (png->openRAM(buf, (int)bufLen, pngDrawCallback) == PNG_SUCCESS) {
    int w = png->getWidth();
    int h = png->getHeight();
    Serial.printf("[SpaceX] PNG source dimensions %dx%d\n", w, h);

    int targetH = 100;
    int targetW = (int)((float)targetH * w / h);
    if (targetW < 1) targetW = 1;

    uint16_t *finalBuf = (uint16_t *)heap_caps_malloc((size_t)targetW * targetH * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    // Root cause of a long-standing heap corruption crash was PNGdec's
    // own getLineAsRGB565() (see pngDrawCallback() above, which now does
    // a manual RGB565 conversion instead) -- with that bypassed, this
    // buffer no longer needs the defensive padding that was tried (and
    // didn't help) while chasing that bug down.
    uint16_t *lineBuf = (uint16_t *)heap_caps_malloc((size_t)w * sizeof(uint16_t), MALLOC_CAP_SPIRAM);

    if (finalBuf != nullptr && lineBuf != nullptr) {
      s_pngFinalBuf = finalBuf;
      s_pngFinalW = targetW;
      s_pngFinalH = targetH;
      s_pngSrcW = w;
      s_pngSrcH = h;
      s_pngLineBuf = lineBuf;
      s_pngObj = png;

      int rc = png->decode(nullptr, 0);

      s_pngFinalBuf = nullptr;
      s_pngLineBuf = nullptr;
      s_pngObj = nullptr;

      if (rc == PNG_SUCCESS) {
        // Same atomic swap as the JPEG path -- see the comment there.
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
        Serial.printf("[SpaceX] PNG decoded %dx%d, downsampled to %dx%d\n", w, h, targetW, targetH);
        success = true;
      } else {
        Serial.printf("[SpaceX] PNG decode failed, rc=%d\n", rc);
        free(finalBuf);
      }
    } else {
      Serial.println("[SpaceX] PNG buffer alloc failed");
      if (finalBuf != nullptr) free(finalBuf);
    }

    if (lineBuf != nullptr) free(lineBuf);
    png->close();
  } else {
    Serial.printf("[SpaceX] PNG openRAM failed, error=%d\n", png->getLastError());
  }

  png->~PNG();
  free(pngMem);
  return success;
}

bool spacex_fetch_next_image() {
  if (!wifi_manager_is_connected()) return false;
  if (g_spacexLaunchCount == 0) return false;
  String url = g_spacexLaunches[0].imageUrl;
  if (url.length() == 0) return false;

  HTTPClient http;
  http.begin(url);
  http.setTimeout(15000); // same fix as the list/landing fetches above
  http.setUserAgent("esp32-home-dashboard/1.0");
  http.useHTTP10(true); // required for the manual read-loop below --
  // without this, chunked transfer encoding corrupts both getSize()
  // (returns -1) and the raw byte stream itself (chunk-size markers
  // get read as image data). Same fix applied elsewhere in this file
  // and across the other fetch services.
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[SpaceX] image fetch HTTP %d\n", code);
    http.end();
    return false;
  }

  // Raised from 250000 -- real LL2 mission photos have been coming back
  // around 660KB, well over the old cap, so the image never once passed
  // this check. 900000 gives headroom above that while still rejecting
  // anything wildly oversized/malformed.
  int len = http.getSize();
  if (len <= 0 || len > 900000) {
    Serial.printf("[SpaceX] image size invalid: %d\n", len);
    http.end();
    return false;
  }

  uint8_t *imgBuf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (imgBuf == nullptr) {
    Serial.println("[SpaceX] image buffer alloc failed");
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t readTotal = 0;
  uint32_t startMs = millis();
  bool readError = false;
  while (http.connected() && readTotal < (size_t)len && millis() - startMs < 15000) {
    size_t avail = stream->available();
    if (avail > 0) {
      int toRead = (int)min((size_t)avail, (size_t)len - readTotal);
      int r = stream->readBytes(imgBuf + readTotal, toRead);
      if (r <= 0) { readError = true; break; }
      readTotal += r;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5)); // yield -- same critical fix used throughout this project
    }
  }
  http.end();

  if (readError || readTotal == 0) {
    Serial.println("[SpaceX] image payload read error");
    free(imgBuf);
    return false;
  }

  // Diagnostic kept from the earlier "JPEG openRAM failed" investigation --
  // it turned out LL2 mission images aren't always JPEG (a real capture
  // showed a PNG signature, 89 50 4E 47, arriving here). This now also
  // drives the format sniff below instead of assuming JPEG.
  Serial.printf("[SpaceX] image read %u of %d bytes, first bytes: %02X %02X %02X %02X\n",
                (unsigned)readTotal, len,
                readTotal > 0 ? imgBuf[0] : 0,
                readTotal > 1 ? imgBuf[1] : 0,
                readTotal > 2 ? imgBuf[2] : 0,
                readTotal > 3 ? imgBuf[3] : 0);

  bool success = false;
  if (readTotal >= 3 && imgBuf[0] == 0xFF && imgBuf[1] == 0xD8 && imgBuf[2] == 0xFF) {
    success = decodeAndStoreJpeg(imgBuf, readTotal);
  } else if (readTotal >= 4 && imgBuf[0] == 0x89 && imgBuf[1] == 0x50 && imgBuf[2] == 0x4E && imgBuf[3] == 0x47) {
    success = decodeAndStorePng(imgBuf, readTotal);
  } else {
    Serial.println("[SpaceX] image format not recognized (not JPEG or PNG)");
  }

  free(imgBuf);
  return success;
}

// Booster landing info for the next launch -- only available via LL2's
// single-launch detail endpoint (not the list/upcoming endpoint used
// above), which returns a much larger response (full agency/rocket/program
// descriptions we don't need, alongside the landing info we do). Reuses
// readHttpBodySafely() since this is JSON text, not binary like the image.
// Returns true only on a fully successful fetch/parse -- see
// spacex_fetch_next_image()'s comment above for why this matters.
bool spacex_fetch_next_landing_info() {
  if (!wifi_manager_is_connected()) return false;
  if (g_spacexLaunchCount == 0) return false;
  String launchId = g_spacexLaunches[0].launchId;
  if (launchId.length() == 0) return false;

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
    return false;
  }

  String payload;
  if (!readHttpBodySafely(http, payload, "SpaceX landing detail")) {
    http.end();
    return false;
  }
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[SpaceX] landing detail JSON parse error: %s\n", err.c_str());
    return false;
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
    return true; // legitimate successful outcome: fetched OK, no landing attempt planned
  }

  JsonObject landing = stages[0]["landing"];
  state_lock();
  g_spacexLandingAttempt = landing["attempt"] | false;
  g_spacexLandingLocation = landing["location"]["name"] | "";
  g_spacexLandingAbbrev = landing["location"]["abbrev"] | "";
  g_spacexLandingType = landing["type"]["abbrev"] | "";
  g_spacexLandingValid = true;
  state_unlock();
  return true;
}

