#include "imagery_service.h"
#include "../assets/imagery_images.h"
#include "../state_mutex.h"
#include <JPEGDEC.h>
#include <esp_heap_caps.h>
#include <new>

uint16_t* g_imageryPixels = nullptr;
int g_imageryWidth = 0;
int g_imageryHeight = 0;
bool g_imageryValid = false;

struct ImageryImage {
  const uint8_t* buf;
  size_t len;
};

// All embedded images must be pre-cropped/resized to the same 600x330
// target (75% of the 800x440 area below the top banner) before being
// converted to a C byte array -- see imagery_images.h. Append new entries
// here as more images are added; imagery_update() picks a random one
// (different from the current one) every 15 minutes.
static const ImageryImage IMAGERY_IMAGES[] = {
  { IMAGERY_1_JPG, IMAGERY_1_JPG_len },
  { IMAGERY_2_JPG, IMAGERY_2_JPG_len },
  { IMAGERY_3_JPG, IMAGERY_3_JPG_len },
  { IMAGERY_4_JPG, IMAGERY_4_JPG_len },
  { IMAGERY_5_JPG, IMAGERY_5_JPG_len },
  { IMAGERY_6_JPG, IMAGERY_6_JPG_len },
  { IMAGERY_7_JPG, IMAGERY_7_JPG_len },
  { IMAGERY_8_JPG, IMAGERY_8_JPG_len },
  { IMAGERY_9_JPG, IMAGERY_9_JPG_len },
  { IMAGERY_10_JPG, IMAGERY_10_JPG_len },
  // IMAGERY_11_JPG and IMAGERY_12_JPG (the two rocket-height-comparison
  // infographics, multiple rockets lined up side by side) removed from
  // rotation per follow-up feedback. Byte arrays still sit unreferenced
  // in imagery_images.h -- left in place rather than editing that large
  // generated file, negligible flash cost either way.
  { IMAGERY_13_JPG, IMAGERY_13_JPG_len },
  { IMAGERY_14_JPG, IMAGERY_14_JPG_len },
  { IMAGERY_15_JPG, IMAGERY_15_JPG_len },
  { IMAGERY_16_JPG, IMAGERY_16_JPG_len },
  { IMAGERY_17_JPG, IMAGERY_17_JPG_len },
  { IMAGERY_18_JPG, IMAGERY_18_JPG_len },
  { IMAGERY_19_JPG, IMAGERY_19_JPG_len },
  { IMAGERY_20_JPG, IMAGERY_20_JPG_len },
  { IMAGERY_21_JPG, IMAGERY_21_JPG_len },
  { IMAGERY_22_JPG, IMAGERY_22_JPG_len },
  { IMAGERY_23_JPG, IMAGERY_23_JPG_len },
  { IMAGERY_24_JPG, IMAGERY_24_JPG_len },
  { IMAGERY_25_JPG, IMAGERY_25_JPG_len },
  { IMAGERY_26_JPG, IMAGERY_26_JPG_len },
  { IMAGERY_27_JPG, IMAGERY_27_JPG_len },
  { IMAGERY_28_JPG, IMAGERY_28_JPG_len },
  { IMAGERY_29_JPG, IMAGERY_29_JPG_len },
  { IMAGERY_30_JPG, IMAGERY_30_JPG_len },
  { IMAGERY_31_JPG, IMAGERY_31_JPG_len },
  { IMAGERY_32_JPG, IMAGERY_32_JPG_len },
  { IMAGERY_33_JPG, IMAGERY_33_JPG_len },
  // IMAGERY_34-37: SpaceX/rocket photos (Dream Chaser night launch, Falcon
  // Heavy on pad, 4-panel SLS/lander/Dream Chaser/Starship collage, Falcon 9
  // booster landing burn). IMAGERY_38-41: Porsche photos (963/GT3 studio,
  // GT3 Cup on track, 911 Targa red, 911 silver profile). IMAGERY_42-43:
  // Starship pad explosion (aerial) and 3-panel explosion sequence.
  { IMAGERY_34_JPG, IMAGERY_34_JPG_len },
  { IMAGERY_35_JPG, IMAGERY_35_JPG_len },
  { IMAGERY_36_JPG, IMAGERY_36_JPG_len },
  { IMAGERY_37_JPG, IMAGERY_37_JPG_len },
  { IMAGERY_38_JPG, IMAGERY_38_JPG_len },
  { IMAGERY_39_JPG, IMAGERY_39_JPG_len },
  { IMAGERY_40_JPG, IMAGERY_40_JPG_len },
  { IMAGERY_41_JPG, IMAGERY_41_JPG_len },
  { IMAGERY_42_JPG, IMAGERY_42_JPG_len },
  { IMAGERY_43_JPG, IMAGERY_43_JPG_len },
};
static const int IMAGERY_IMAGE_COUNT = sizeof(IMAGERY_IMAGES) / sizeof(IMAGERY_IMAGES[0]);

static int s_currentImageIndex = -1;

static uint16_t* s_decodeTarget = nullptr;
static int s_decodeTargetW = 0;
static int s_decodeTargetH = 0;

// Yields periodically during decode, same defensive fix as the SpaceX PNG
// flicker bug -- a 330-row full decode is a smaller burst than that PNG's
// 722 rows, but still large enough to risk the same PSRAM/display-DMA
// contention now that this runs periodically (every 15 min) rather than
// once at boot like the old single easter-egg image did.
static int imageryDrawCallback(JPEGDRAW *pDraw) {
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

// Decodes IMAGERY_IMAGES[index] into g_imageryPixels. JPEGDEC is heap-
// allocated on PSRAM rather than declared as a local (stack) variable --
// same fix as the original easter-egg boot-loop crash, since this can be
// called from setup() (Arduino's loopTask, small stack) as well as the
// network task.
static bool decodeImageryImage(int index) {
  const ImageryImage &img = IMAGERY_IMAGES[index];

  JPEGDEC *jpeg = (JPEGDEC *)heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_SPIRAM);
  if (jpeg == nullptr) {
    Serial.println("[Imagery] JPEGDEC alloc failed");
    return false;
  }
  new (jpeg) JPEGDEC();

  bool success = false;
  if (jpeg->openRAM(const_cast<uint8_t*>(img.buf), (int)img.len, imageryDrawCallback)) {
    int w = jpeg->getWidth();
    int h = jpeg->getHeight();

    // Allocated once and reused for every subsequent rotation (all images
    // are pre-sized to the same 600x330 target), so this only runs on the
    // very first decode.
    if (g_imageryPixels == nullptr) {
      g_imageryPixels = (uint16_t *)heap_caps_malloc((size_t)w * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    }

    if (g_imageryPixels != nullptr) {
      state_lock(); // guards against the UI task reading this buffer mid-decode
      s_decodeTarget = g_imageryPixels;
      s_decodeTargetW = w;
      s_decodeTargetH = h;
      jpeg->decode(0, 0, 0);
      s_decodeTarget = nullptr;

      g_imageryWidth = w;
      g_imageryHeight = h;
      g_imageryValid = true;
      state_unlock();
      success = true;
      Serial.printf("[Imagery] decoded image %d/%d (%dx%d)\n", index + 1, IMAGERY_IMAGE_COUNT, w, h);
    } else {
      Serial.println("[Imagery] pixel buffer alloc failed");
    }
    jpeg->close();
  } else {
    Serial.println("[Imagery] JPEG openRAM failed");
  }

  jpeg->~JPEGDEC();
  free(jpeg);
  return success;
}

void imagery_init() {
  s_currentImageIndex = 0;
  decodeImageryImage(s_currentImageIndex);
}

void imagery_update() {
  if (IMAGERY_IMAGE_COUNT <= 1) return; // nothing to rotate to yet -- still just the placeholder
  int next = s_currentImageIndex;
  while (next == s_currentImageIndex) {
    next = random(0, IMAGERY_IMAGE_COUNT);
  }
  s_currentImageIndex = next;
  decodeImageryImage(s_currentImageIndex);
}
