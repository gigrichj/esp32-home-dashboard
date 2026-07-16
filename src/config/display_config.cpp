#include "display_config.h"
#include <Arduino.h>
#include <lvgl.h>
#include <esp_display_panel.hpp>
#include <esp_heap_caps.h>

using namespace esp_panel::board;
using namespace esp_panel::drivers;

static Board* board = nullptr;
static LCD* lcd = nullptr;
static Touch* touch = nullptr;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t* buf1 = nullptr;
static lv_color_t* buf2 = nullptr;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

static void disp_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
  if (lcd != nullptr) {
    lcd->drawBitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, (const void*)color_p);
  }
  lv_disp_flush_ready(drv);
}

static void touch_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  if (touch == nullptr) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }
  data->state = LV_INDEV_STATE_REL;
}

void display_init() {
  lv_init();

  Serial.println("[display] Board init begin");
  board = new Board();
  if (!board->init()) {
    Serial.println("[display] board->init() failed");
    return;
  }

  lcd = board->getLCD();
  if (lcd != nullptr) {
    lcd->configFrameBufferNumber(2);
  }

  if (!board->begin()) {
    Serial.println("[display] board->begin() failed");
    return;
  }

  lcd = board->getLCD();
  touch = board->getTouch();
  Serial.printf("[display] ready: %dx%d, touch=%d\n",
                lcd ? lcd->getFrameWidth() : -1,
                lcd ? lcd->getFrameHeight() : -1,
                touch != nullptr ? 1 : 0);

  const uint32_t buf_pixels = SCREEN_WIDTH * (SCREEN_HEIGHT / 10);
  buf1 = (lv_color_t*)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  buf2 = (lv_color_t*)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_pixels);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_WIDTH;
  disp_drv.ver_res = SCREEN_HEIGHT;
  disp_drv.flush_cb = disp_flush_cb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read_cb;
  lv_indev_drv_register(&indev_drv);
}

void display_loop() {
}
