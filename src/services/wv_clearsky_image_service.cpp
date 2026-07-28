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
// Left-edge crop offset (in decoded source-pixel space) -- shifts every
// column left by this amount before jpegDrawCallback's existing bounds
// check runs, so columns that land negative (the cropped-off left
// region) get dropped by the same mechanism already used for the
// right-edge crop, just applied from the other direction.
static int s_decodeLeftCropOffset = 0;

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
      int destX = pDraw->x + col - s_decodeLeftCropOffset;
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
    // Left crop added: 0.075 * w (~0.75in equivalent) trimmed off the
    // left edge to remove the source chart's own blurry embedded row
    // labels (illegible at this display resolution) -- replaced with our
    // own crisp labels drawn separately in screen_manager.cpp, in a
    // dedicated column reserved outside the image itself. Combined with
    // the existing 12.5% right crop, 80% of the source width is kept.
    // Right crop made an explicit named fraction (was previously just
    // implied by decodedW=0.80w combined with the old 0.075 left crop --
    // 0.075+0.80=0.875, i.e. an unnamed 12.5% off the right). Kept at
    // the same 0.125 value so the right edge doesn't move.
    //
    // leftCropFrac increased from 0.075 to 0.10 -- 0.075 still left a
    // sliver of the source chart's own embedded label column visible (a
    // gray legend box and partial ghosted text behind our overlaid
    // labels, per on-device photo). decodedW now derives from both
    // fractions so only the left edge of the crop window moves; decode
    // scale (full JPEG_SCALE, no downscale) and croppedH (bottom crop)
    // are both untouched, so resolution/height stay exactly as before.
    // If leftCropFrac overshoots and starts cutting real chart columns,
    // back off toward 0.075 again.
    float leftCropFrac = 0.10f;
    float rightCropFrac = 0.125f;
    s_decodeLeftCropOffset = (int)(w * leftCropFrac);
    int decodedW = (int)(w * (1.0f - leftCropFrac - rightCropFrac));
    int decodedH = croppedH;

    size_t decodeBufBytes = (size_t)decodedW * decodedH * sizeof(uint16_t);
    Serial.printf("[WV ClearSky] requesting %u bytes for decode buffer, free PSRAM: %u bytes\n",
                  (unsigned)decodeBufBytes, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    uint16_t *decodeBuf = (uint16_t *)heap_caps_malloc(decodeBufBytes, MALLOC_CAP_SPIRAM);
    if (decodeBuf != nullptr) {
      s_decodeTarget = decodeBuf;
      s_decodeTargetW = decodedW;
      s_decodeTargetH = decodedH;
      jpeg.decode(0, 0, 0);
      s_decodeTarget = nullptr;
      s_decodeLeftCropOffset = 0;

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
      // Reverted back to the full 800px width -- the reserved-column
      // approach (shrinking this to 640 and moving the image right) was
      // undone per follow-up feedback; the left crop above already
      // removes the source chart's own embedded label column, and our
      // own replacement labels are now drawn overlaid on the image
      // itself (screen_manager.cpp) rather than in a separate reserved
      // strip -- same "crop the source, stretch across the full display
      // width" pattern already used for the right-edge crop.
      int targetW = 800;
      int targetH = (int)((float)targetW * croppedH / decodedW);
      if (targetH < 1) targetH = 1;
      // Raised from 190 to 200 -- the version number that used to sit
      // right at chartY+190 has been removed from this page (see
      // screen_manager.cpp), freeing genuine room down toward the
      // actual screen edge, not just an arbitrary bump.
      if (targetH > 200) targetH = 200;

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

  // Diagnostic: free PSRAM at the start of this fetch, to help find the
  // safe width ceiling empirically -- w=2400 caused a real on-device
  // failure (see the w=1800 comment below), and rather than keep
  // guessing widths, logging actual free memory here lets us calculate
  // margin directly instead of trial-and-error flashing.
  Serial.printf("[WV ClearSky] free PSRAM before fetch: %u bytes\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

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
  // q=85 (reverted from 100) -- diagnostics on-device ruled out the
  // memory-exhaustion theory (3.4MB free PSRAM, and the fetched file
  // itself was a fully-read, valid JPEG: readTotal matched Content-
  // Length exactly, magic number FF D8 FF DB) -- yet jpeg.openRAM()
  // still failed on a 638KB file. That's a huge file for this decoder
  // (SpaceX images are typically ~100KB), and JPEGDEC needs its own
  // internal working buffers (Huffman tables, MCU state) separate from
  // the PSRAM output buffer we control -- likely allocated from the
  // much smaller internal SRAM pool, not the PSRAM this diagnostic
  // measured. q=100 is almost certainly why the file ballooned (JPEG
  // compression gets dramatically less efficient near-lossless,
  // especially on hard block edges like this chart). Reverting to the
  // known-good q=85 to test that theory directly, one variable at a
  // time, while keeping the width/sharpen changes.
  String sourceUrl = "https://www.cleardarksky.com/c/spruce_WVcsk.gif?c=2372454";
  String proxiedUrl = "https://wsrv.nl/?url=" + urlEncode(sourceUrl) + "&output=jpg&w=1800&q=85&sharp=10";

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

  // Diagnostic: raw byte count read vs. the Content-Length the server
  // reported, plus the first 4 bytes -- same pattern already used in
  // spacex_launch_service.cpp. A valid JPEG starts with FF D8 FF. If
  // readTotal doesn't match len, or the first bytes aren't a JPEG magic
  // number, the fetched data itself is bad -- a different problem than
  // the memory-exhaustion one previously assumed, which the last flash's
  // log (plenty of free PSRAM right before the openRAM failure) now
  // rules out as the actual cause.
  Serial.printf("[WV ClearSky] image read %u of %d bytes, first bytes: %02X %02X %02X %02X\n",
                (unsigned)readTotal, len,
                readTotal > 0 ? imgBuf[0] : 0,
                readTotal > 1 ? imgBuf[1] : 0,
                readTotal > 2 ? imgBuf[2] : 0,
                readTotal > 3 ? imgBuf[3] : 0);

  bool ok = decodeAndStoreClearSkyJpeg(imgBuf, readTotal);
  free(imgBuf);
  return ok;
}
