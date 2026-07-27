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

    // JPEG_SCALE_QUARTER instead of EIGHTH -- with the actual source
    // dimensions logged on-device (1600x364), an eighth-scale decode
    // (200x45) had to be upscaled ~3.7x to reach the ~747x170 render
    // size, producing visibly blocky output. Quarter-scale (400x91) cuts
    // that to ~1.87x upscale while keeping row count (~91) close to the
    // SpaceX page's own established ~90-row precedent for this decode
    // pattern, so it shouldn't reintroduce the flicker that full-
    // resolution PNG decodes caused elsewhere in this project.
    int decodedW = w / 4;
    int decodedH = h / 4;

    uint16_t *decodeBuf = (uint16_t *)heap_caps_malloc((size_t)decodedW * decodedH * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (decodeBuf != nullptr) {
      s_decodeTarget = decodeBuf;
      s_decodeTargetW = decodedW;
      s_decodeTargetH = decodedH;
      jpeg.decode(0, 0, JPEG_SCALE_QUARTER);
      s_decodeTarget = nullptr;

      // Target box for the previously-empty space below the 5-day
      // column strip on the WV Astro page. Height picked to fit that
      // space; width derived from the real source aspect ratio rather
      // than assumed, capped to the page's usable width.
      int targetH = 170;
      int targetW = (int)((float)targetH * w / h);
      if (targetW < 1) targetW = 1;
      // BUG FIX: clamping targetW without also shrinking targetH would
      // squash/distort the image if the source is wider than this page
      // has room for -- recompute targetH from the clamped width instead
      // so the aspect ratio is preserved (smaller image, not a squished one).
      if (targetW > 760) {
        targetW = 760;
        targetH = (int)((float)targetW * h / w);
        if (targetH < 1) targetH = 1;
      }

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
  // as spacex_launch_service.cpp's PNG->JPEG conversion. w=1600 is a
  // starting guess for JPEGDEC's 1/8-scale decode requirement -- the
  // Serial log line above ("JPEG source dimensions...") will show the
  // real source size on first successful fetch, which may mean this
  // width needs tuning once we see actual on-device numbers (the Clear
  // Sky Chart's real aspect ratio -- wide, multi-day hour grid -- is
  // quite different from the SpaceX mission photos this pattern was
  // originally built for).
  String sourceUrl = "https://www.cleardarksky.com/c/spruce_WVcsk.gif?c=2372454";
  String proxiedUrl = "https://wsrv.nl/?url=" + urlEncode(sourceUrl) + "&output=jpg&w=1600&q=85";

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
