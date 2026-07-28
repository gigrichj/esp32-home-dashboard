#include "world_trips_banner_service.h"
#include "../assets/world_trips_banners.h"
#include "../state_mutex.h"
#include <JPEGDEC.h>
#include <esp_heap_caps.h>
#include <new>

uint16_t* g_worldTripsBannerPixels = nullptr;
int g_worldTripsBannerWidth = 0;
int g_worldTripsBannerHeight = 0;
bool g_worldTripsBannerValid = false;

struct WorldTripsBannerImage {
  const uint8_t* buf;
  size_t len;
};

// All embedded images are pre-resized by the user to exactly 800x160
// (this project's established banner slot size) before being converted to
// a C byte array -- see world_trips_banners.h. Append new entries here as
// more are added; world_trips_banner_update() picks a random one
// (different from the current one) every WORLD_TRIPS_BANNER_ROTATE_MS.
static const WorldTripsBannerImage WORLD_TRIPS_BANNER_IMAGES[] = {
  { WORLD_TRIPS_BANNER_1_JPG, WORLD_TRIPS_BANNER_1_JPG_len },
  { WORLD_TRIPS_BANNER_2_JPG, WORLD_TRIPS_BANNER_2_JPG_len },
  { WORLD_TRIPS_BANNER_3_JPG, WORLD_TRIPS_BANNER_3_JPG_len },
  { WORLD_TRIPS_BANNER_4_JPG, WORLD_TRIPS_BANNER_4_JPG_len },
  { WORLD_TRIPS_BANNER_5_JPG, WORLD_TRIPS_BANNER_5_JPG_len },
  { WORLD_TRIPS_BANNER_6_JPG, WORLD_TRIPS_BANNER_6_JPG_len },
  { WORLD_TRIPS_BANNER_7_JPG, WORLD_TRIPS_BANNER_7_JPG_len },
  { WORLD_TRIPS_BANNER_8_JPG, WORLD_TRIPS_BANNER_8_JPG_len },
};
static const int WORLD_TRIPS_BANNER_IMAGE_COUNT = sizeof(WORLD_TRIPS_BANNER_IMAGES) / sizeof(WORLD_TRIPS_BANNER_IMAGES[0]);

static int s_currentBannerIndex = -1;

static uint16_t* s_decodeTarget = nullptr;
static int s_decodeTargetW = 0;
static int s_decodeTargetH = 0;

// Same defensive periodic-yield pattern as imagery_service.cpp's decode
// callback -- guards against PSRAM/display-DMA contention during the
// decode burst.
static int worldTripsBannerDrawCallback(JPEGDRAW *pDraw) {
  if (s_decodeTarget == nullptr) return 0;
  static int rowsSinceYield = 0;
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
    rowsSinceYield++;
    if (rowsSinceYield >= 20) {
      rowsSinceYield = 0;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  return 1;
}

// Decodes WORLD_TRIPS_BANNER_IMAGES[index] into g_worldTripsBannerPixels.
// JPEGDEC is heap-allocated on PSRAM rather than a stack local -- same fix
// as imagery_service.cpp's decodeImageryImage(), since this can run from
// setup() (Arduino's loopTask, small stack) as well as the network task.
static bool decodeWorldTripsBannerImage(int index) {
  const WorldTripsBannerImage &img = WORLD_TRIPS_BANNER_IMAGES[index];

  JPEGDEC *jpeg = (JPEGDEC *)heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_SPIRAM);
  if (jpeg == nullptr) {
    Serial.println("[WorldTripsBanner] JPEGDEC alloc failed");
    return false;
  }
  new (jpeg) JPEGDEC();

  bool success = false;
  if (jpeg->openRAM(const_cast<uint8_t*>(img.buf), (int)img.len, worldTripsBannerDrawCallback)) {
    int w = jpeg->getWidth();
    int h = jpeg->getHeight();

    // Allocated once and reused for every subsequent rotation (all images
    // are pre-sized to the same 800x160 target), same as imagery_service.cpp.
    if (g_worldTripsBannerPixels == nullptr) {
      g_worldTripsBannerPixels = (uint16_t *)heap_caps_malloc((size_t)w * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    }

    if (g_worldTripsBannerPixels != nullptr) {
      state_lock(); // guards against the UI task reading this buffer mid-decode
      s_decodeTarget = g_worldTripsBannerPixels;
      s_decodeTargetW = w;
      s_decodeTargetH = h;
      jpeg->decode(0, 0, 0); // full resolution -- source is already pre-sized to the target display size
      s_decodeTarget = nullptr;

      g_worldTripsBannerWidth = w;
      g_worldTripsBannerHeight = h;
      g_worldTripsBannerValid = true;
      state_unlock();
      success = true;
      Serial.printf("[WorldTripsBanner] decoded image %d/%d (%dx%d)\n", index + 1, WORLD_TRIPS_BANNER_IMAGE_COUNT, w, h);
    } else {
      Serial.println("[WorldTripsBanner] pixel buffer alloc failed");
    }
    jpeg->close();
  } else {
    Serial.println("[WorldTripsBanner] JPEG openRAM failed");
  }

  jpeg->~JPEGDEC();
  free(jpeg);
  return success;
}

void world_trips_banner_init() {
  s_currentBannerIndex = 0;
  decodeWorldTripsBannerImage(s_currentBannerIndex);
}

void world_trips_banner_update() {
  if (WORLD_TRIPS_BANNER_IMAGE_COUNT <= 1) return; // nothing to rotate to
  int next = s_currentBannerIndex;
  while (next == s_currentBannerIndex) {
    next = random(0, WORLD_TRIPS_BANNER_IMAGE_COUNT);
  }
  s_currentBannerIndex = next;
  decodeWorldTripsBannerImage(s_currentBannerIndex);
}
