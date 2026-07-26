#include "debug_easter_egg.h"
#include "../assets/debug_easter_egg_image.h"
#include <JPEGDEC.h>
#include <esp_heap_caps.h>

uint16_t* g_debugEasterEggPixels = nullptr;
int g_debugEasterEggWidth = 0;
int g_debugEasterEggHeight = 0;
bool g_debugEasterEggValid = false;

static uint16_t* s_decodeTarget = nullptr;
static int s_decodeTargetW = 0;
static int s_decodeTargetH = 0;

static int debugEasterEggDrawCallback(JPEGDRAW *pDraw) {
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

// One-time full-resolution decode (no 1/8-scale trick needed -- the
// embedded source is already 344x200, matching the DEBUG page target box)
// of the embedded easter-egg JPEG, called once from setup() before the UI
// task starts drawing pages. No network fetch, no PSRAM/DMA contention
// concerns like the live SpaceX/aviation image paths -- this happens once
// at boot, not repeatedly on a poll interval.
void debug_easter_egg_init() {
  JPEGDEC jpeg;
  if (jpeg.openRAM(const_cast<uint8_t*>(DEBUG_EASTER_EGG_JPG), (int)DEBUG_EASTER_EGG_JPG_len, debugEasterEggDrawCallback)) {
    int w = jpeg.getWidth();
    int h = jpeg.getHeight();

    uint16_t *buf = (uint16_t *)heap_caps_malloc((size_t)w * h * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (buf != nullptr) {
      s_decodeTarget = buf;
      s_decodeTargetW = w;
      s_decodeTargetH = h;
      jpeg.decode(0, 0, 0);
      s_decodeTarget = nullptr;

      g_debugEasterEggPixels = buf;
      g_debugEasterEggWidth = w;
      g_debugEasterEggHeight = h;
      g_debugEasterEggValid = true;
      Serial.printf("[Debug] easter egg image decoded %dx%d\n", w, h);
    } else {
      Serial.println("[Debug] easter egg pixel buffer alloc failed");
    }
    jpeg.close();
  } else {
    Serial.println("[Debug] easter egg JPEG openRAM failed");
  }
}
