#pragma once
#include <Arduino.h>

// Fetches and decodes the Spruce Knob Mountain Center Clear Sky Chart --
// the classic astronomer's forecast grid (cloud/transparency/seeing/
// darkness rows, one column per hour) -- for display on the WV Astro
// page. The source image is a GIF, which this project's image pipeline
// can't decode directly (JPEG/PNG only), so it's routed through wsrv.nl
// the same established way spacex_launch_service.cpp already does for
// PNG mission photos: fetch, convert format server-side, get back a
// plain JPEG. See wv_clearsky_image_service.cpp for the fetch URL.
extern uint16_t* g_wvClearSkyImagePixels;
extern int g_wvClearSkyImageWidth;
extern int g_wvClearSkyImageHeight;
extern bool g_wvClearSkyImageValid;
extern int g_wvClearSkyImageLastHttpCode;

// Returns true only on a fully successful fetch + decode.
bool wv_clearsky_fetch_image();
