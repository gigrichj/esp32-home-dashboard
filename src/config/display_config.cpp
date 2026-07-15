#include "display_config.h"
#include <lvgl.h>
#include <esp_display_panel.hpp>
#include <lvgl_port_v8.h>

using namespace esp_panel::board;

static Board* board = nullptr;

void display_init() {
  board = new Board();
  board->init();
  board->begin();

  lvgl_port_init(board->getLCD(), board->getTouch());
}

void display_loop() {
}
