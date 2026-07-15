#include "screen_manager.h"
#include <lvgl.h>
#include "../services/weather_service.h"
#include "../services/smarthome_service.h"
#include "../services/iss_service.h"

static lv_obj_t* tabview;

// Labels that get updated on refresh — kept as static pointers so
// screen_manager_refresh() doesn't need to re-walk the widget tree.
static lv_obj_t* lblTime;
static lv_obj_t* lblWeather;
static lv_obj_t* lblIss;
static lv_obj_t* lblHouseStatus;

static void build_dashboard_tab(lv_obj_t* parent) {
  lblTime = lv_label_create(parent);
  lv_obj_set_style_text_font(lblTime, &lv_font_montserrat_28, 0);
  lv_obj_align(lblTime, LV_ALIGN_TOP_LEFT, 10, 10);
  lv_label_set_text(lblTime, "--:--");

  lblWeather = lv_label_create(parent);
  lv_obj_align(lblWeather, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_label_set_text(lblWeather, "Weather: --");

  lblIss = lv_label_create(parent);
  lv_obj_align(lblIss, LV_ALIGN_LEFT_MID, 10, 0);
  lv_label_set_text(lblIss, "ISS: --");

  lblHouseStatus = lv_label_create(parent);
  lv_obj_align(lblHouseStatus, LV_ALIGN_BOTTOM_LEFT, 10, -10);
  lv_label_set_text(lblHouseStatus, "House: --");

  // TODO: Porsche connection status, aircraft-nearby count, and
  // notifications feed follow the same
  // "lv_label_create + align + store pointer" pattern as above.
}

static void build_placeholder_tab(lv_obj_t* parent, const char* name) {
  lv_obj_t* lbl = lv_label_create(parent);
  lv_label_set_text_fmt(lbl, "%s\n(screen not built yet)", name);
  lv_obj_center(lbl);
}

void screen_manager_init() {
  tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 50);

  lv_obj_t* tDash   = lv_tabview_add_tab(tabview, "Dashboard");
  lv_obj_t* tAvi    = lv_tabview_add_tab(tabview, "Aviation");
  lv_obj_t* tCar    = lv_tabview_add_tab(tabview, "Porsche");
  lv_obj_t* tHome   = lv_tabview_add_tab(tabview, "Smart Home");
  lv_obj_t* tIss    = lv_tabview_add_tab(tabview, "ISS");
  lv_obj_t* tWx     = lv_tabview_add_tab(tabview, "Weather");

  build_dashboard_tab(tDash);
  build_placeholder_tab(tAvi,  "Aviation / Radar");
  build_placeholder_tab(tCar,  "Porsche");
  build_placeholder_tab(tHome, "Smart Home");
  build_placeholder_tab(tIss,  "ISS Tracker");
  build_placeholder_tab(tWx,   "Weather Detail");
}

void screen_manager_refresh() {
  if (g_weather.valid) {
    lv_label_set_text_fmt(lblWeather, "%.0f°F  %s", g_weather.tempF, g_weather.condition.c_str());
  }
  if (g_iss.valid) {
    lv_label_set_text_fmt(lblIss, "ISS alt: %.0f km", g_iss.altitudeKm);
  }
  if (g_deviceCount > 0) {
    lv_label_set_text_fmt(lblHouseStatus, "House: %d devices online", g_deviceCount);
  }
}
