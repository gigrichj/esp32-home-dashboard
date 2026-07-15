#include "display_config.h"
#include <lvgl.h>
#include <esp_heap_caps.h>

static lv_disp_draw_buf_t draw_buf;
static lv_color_t* buf1 = nullptr;
static lv_color_t* buf2 = nullptr;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

static void disp_flush_stub(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
  lv_disp_flush_ready(drv);
}

static void touch_read_stub(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  data->state = LV_INDEV_STATE_REL;
}

void display_init() {
  lv_init();

  const uint32_t buf_pixels = SCREEN_WIDTH * (SCREEN_HEIGHT / 10);
  buf1 = (lv_color_t*)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  buf2 = (lv_color_t*)heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_pixels);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_WIDTH;
  disp_drv.ver_res = SCREEN_HEIGHT;
  disp_drv.flush_cb = disp_flush_stub;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read_stub;
  lv_indev_drv_register(&indev_drv);
}

void display_loop() {
}
