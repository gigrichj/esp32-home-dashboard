#include "wv_clearsky_image_service.h"
#include "wifi_manager.h"
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <JPEGDEC.h>
#include "../state_mutex.h"

uint16_t* g_wvClearSkyImagePixels = nullptr;
int g_wvClearSkyImageWidth = 0;
int g_wvClearSkyImageHeight = 0;
bool g_wvClearSkyImageValid = false;
int g_wvClearSkyImageLastHttpCode = -999;

// Same jpegDrawCallback/s_decodeTarget pattern used in
// spacex_launch_service.cpp and aviation_service.cpp -- file-scoped
// statics here don't collide with those files' own copies, so this is a
// straightforward duplication rather than a shared/refactored helper
// (matching this project's established convention for this pattern).
static uint16_t* s_decodeTarget = nullptr;
static int s_decodeTargetW = 0;
static int s_decodeTargetH = 0;

// Periodic yield counter -- HALF-scale decode processes roughly double
// the rows that EIGHTH/QUARTER-scale did, edging closer to the PSRAM-
// contention territory that caused this project's original display-
// flicker bug (see pngDrawCallback()'s history elsewhere in this
// project). Yielding every few callback invocations gives the display
// DMA regular breathing room during the decode, same fix pattern, added
// here preemptively rather than waiting to confirm a regression.
static int s_yieldCounter = 0;

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
  s_yieldCounter++;
  if (s_yieldCounter >= 2) {
    s_yieldCounter = 0;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return 1;
}

// Same percent-encoding helper as spacex_launch_service.cpp's urlEncode()
// -- needed to safely nest the Clear Sky Chart's own URL (which has a
// query string) inside wsrv.nl's url= parameter.
static String urlEncode(const String &s) {
  String encoded = "";
  char buf[4];
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}

// Same two-stage shrink as spacex_launch_service.cpp's
// decodeAndStoreJpeg() (JPEG_SCALE_EIGHTH, then manual nearest-neighbor
// downsample to the on-screen target box) -- JPEG only, no PNG path
// needed since wsrv.nl's &output=jpg parameter guarantees a JPEG response
// regardless of the source GIF format.
static bool decodeAndStoreClearSkyJpeg(uint8_t *buf, size_t bufLen) {
  bool success = false;

  JPEGDEC jpeg;
  if (jpeg.openRAM(buf, (int)bufLen, jpegDrawCallback)) {
    int w = jpeg.getWidth();
    int h = jpeg.getHeight();
    Serial.printf("[WV ClearSky] JPEG source dimensions %dx%d\n", w, h);

    // HALF-scale instead of QUARTER -- extracts genuinely more real
    // detail from the same already-fetched 1600x364 JPEG (not just
    // re-interpolating), landing decodedW/H close enough to the final
    // target that little to no upscaling is needed. See jpegDrawCallback()
    // above for the periodic yield added alongside this change (HALF-scale
    // roughly doubles the row count vs QUARTER, so the yield is a
    // preemptive safety measure against reintroducing display flicker).
    //
    // Also crops the bottom ~8% of the source (a small copyright/legend
    // text line baked into the original chart) -- works by decoding into
    // a buffer shorter than the full decoded height; jpegDrawCallback
    // already drops any row past s_decodeTargetH, so no new crop logic
    // is needed, just a smaller target height computed from croppedH.
    // Backed off from 0.81 -- that cropped too far, cutting the
    // Humidity row (only Smoke/Wind survived). 0.90 keeps one more row
    // while chartY in screen_manager.cpp is moved up to compensate for
    // the added height, so this still clears the diagnostic HTTP-code
    // line drawn beneath the image.
    float cropKeepFraction = 0.90f;
    int croppedH = (int)(h * cropKeepFraction);

    // Full resolution (no JPEGDEC scale-down at all) -- extracts every
    // bit of real detail already present in the fetched 1600px-wide
    // JPEG, the maximum possible sharpness without fetching a larger
    // source image. Roughly doubles the row count again vs HALF-scale
    // (~148 -> ~295 after the crop), so the yield frequency below is
    // tightened accordingly as extra insurance against the flicker this
    // decode-volume class of bug caused before.
    // Right-edge crop expressed as a fraction of source width (7.5%,
    // matching the previous cumulative 3/4in crop at the old 1600px
    // fetch width) rather than a fixed pixel count -- now that the
    // source fetch width itself changed (1600 -> 2400), a flat "120px"
    // offset would represent a smaller, wrong proportion of the new,
    // wider source. This scales correctly regardless of what width we
    // request going forward. Decode is still full resolution -- no
    // JPEGDEC scale-down at all -- so this remains the maximum real
    // detail available from whatever gets fetched.
    int decodedW = (int)(w * 0.925f);
    int decodedH = croppedH;

    uint16_t *decodeBuf = (uint16_t *)heap_caps_malloc((size_t)decodedW * decodedH * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (decodeBuf != nullptr) {
      s_decodeTarget = decodeBuf;
      s_decodeTargetW = decodedW;
      s_decodeTargetH = decodedH;
      jpeg.decode(0, 0, 0);
      s_decodeTarget = nullptr;

      // Target box widened to use the visible space on the right side of
      // the page (previously only ~747px of the available ~780px) --
      // width is now the primary dimension, height follows from the
      // CROPPED aspect ratio so the kept portion fills the frame with no
      // blank gap where the cropped legend line used to be.
      // Full screen width (800px), edge-to-edge with the top banner --
      // matches the banner's own x=0 to x=WIDTH span exactly. At the
      // source's native 1600px width, this is a PRECISE 2.0x downsize
      // ratio (1600/800), not an arbitrary fraction -- and since
      // croppedH/2 lands on a whole number too, both dimensions shrink
      // by the exact same clean 2.0x factor. No mismatched down-then-up
      // scaling anywhere: this decode is already at full native
      // resolution (JPEG_SCALE full, not EIGHTH/QUARTER/HALF), so every
      // pixel written to the final buffer is a real downsample, never
      // an upsample guess.
      int targetW = 800;
      int targetH = (int)((float)targetW * croppedH / decodedW);
      if (targetH < 1) targetH = 1;
      if (targetH > 170) targetH = 170; // safety cap -- keeps clear of the
                                          // diagnostic HTTP-code line drawn
                                          // below the image on the page

      uint16_t *finalBuf = (uint16_t *)heap_caps_malloc((size_t)targetW * targetH * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
      if (finalBuf != nullptr) {
        for (int dy = 0; dy < targetH; dy++) {
          int srcY = dy * decodedH / targetH;
          for (int dx = 0; dx < targetW; dx++) {
            int srcX = dx * decodedW / targetW;
            finalBuf[dy * targetW + dx] = decodeBuf[srcY * decodedW + srcX];
          }
        }

        // Locked as one atomic swap -- same reasoning as
        // spacex_launch_service.cpp's decodeAndStoreJpeg(): if the UI
        // task's drawRGBBitmap() read this pointer mid-swap, it would
        // dereference freed memory.
        state_lock();
        if (g_wvClearSkyImagePixels != nullptr) {
          free(g_wvClearSkyImagePixels);
          g_wvClearSkyImagePixels = nullptr;
          g_wvClearSkyImageValid = false;
        }
        g_wvClearSkyImagePixels = finalBuf;
        g_wvClearSkyImageWidth = targetW;
        g_wvClearSkyImageHeight = targetH;
        g_wvClearSkyImageValid = true;
        state_unlock();
        Serial.printf("[WV ClearSky] JPEG decoded %dx%d, downsampled to %dx%d\n", decodedW, decodedH, targetW, targetH);
        success = true;
      } else {
        Serial.printf("[WV ClearSky] final image buffer alloc failed (%dx%d requested)\n", targetW, targetH);
      }

      free(decodeBuf);
    } else {
      Serial.printf("[WV ClearSky] intermediate decode buffer alloc failed (%dx%d requested)\n", decodedW, decodedH);
    }
    jpeg.close();
  } else {
    Serial.println("[WV ClearSky] JPEG openRAM failed");
  }

  return success;
}

bool wv_clearsky_fetch_image() {
  if (!wifi_manager_is_connected()) return false;

  // Spruce Knob Mountain Center Clear Sky Chart -- source is a GIF,
  // routed through wsrv.nl to get a JPEG back, same established pattern
  // as spacex_launch_service.cpp's PNG->JPEG conversion.
  // w=2400 (up from 1600) -- testing whether the true underlying chart
  // has more real detail than the previous request captured; check the
  // "JPEG source dimensions" Serial log line to see what actually comes
  // back. q=100 (up from 85) -- this chart is solid-color rectangles,
  // not a photo, so JPEG's lossy compression was likely softening the
  // hard edges between blocks; max quality should sharpen those edges
  // at the cost of a somewhat larger fetch.
  // w=1800 (backed off from 2400) -- the 2400 request produced a
  // ~2.2MB full-resolution decode buffer (single contiguous PSRAM
  // allocation), which correlated with a real on-device failure: a
  // "[WV ClearSky] JPEG openRAM failed" immediately preceded by an
  // unrelated ISS fetch failing with an SSL memory allocation error a
  // few seconds earlier -- both consistent with PSRAM exhaustion/
  // fragmentation from that one oversized buffer. 1800 keeps most of
  // the quality/sharpen gains while cutting the decode buffer to a
  // safer size.
  String sourceUrl = "https://www.cleardarksky.com/c/spruce_WVcsk.gif?c=2372454";
  String proxiedUrl = "https://wsrv.nl/?url=" + urlEncode(sourceUrl) + "&output=jpg&w=1800&q=100&sharp=5";

  HTTPClient http;
  http.begin(proxiedUrl);
  http.setTimeout(15000);
  http.setUserAgent("esp32-home-dashboard/1.0");
  http.useHTTP10(true); // same chunked-transfer fix used everywhere else in this project
  int code = http.GET();
  g_wvClearSkyImageLastHttpCode = code;
  if (code != 200) {
    Serial.printf("[WV ClearSky] image fetch HTTP %d\n", code);
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len <= 0 || len > 900000) {
    Serial.printf("[WV ClearSky] image size invalid: %d\n", len);
    http.end();
    return false;
  }

  uint8_t *imgBuf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (imgBuf == nullptr) {
    Serial.println("[WV ClearSky] image buffer alloc failed");
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
    Serial.println("[WV ClearSky] image payload read error");
    free(imgBuf);
    return false;
  }

  bool ok = decodeAndStoreClearSkyJpeg(imgBuf, readTotal);
  free(imgBuf);
  return ok;
}
