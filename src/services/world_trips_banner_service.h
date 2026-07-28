#pragma once
#include <stdint.h>

// Decodes the first embedded World Trips banner image
// (assets/world_trips_banners.h) at boot -- pure local decode, no network
// dependency, same pattern as imagery_init().
void world_trips_banner_init();

// Rotates to a different (random, never repeating the current one) banner
// image from the embedded set. Call periodically (every
// WORLD_TRIPS_BANNER_ROTATE_MS, see main.cpp) -- same pattern as
// imagery_update()'s 15-minute rotation, just on a slower 3-hour cadence
// since these banners change far less often.
void world_trips_banner_update();

extern uint16_t* g_worldTripsBannerPixels;
extern int g_worldTripsBannerWidth;
extern int g_worldTripsBannerHeight;
extern bool g_worldTripsBannerValid;
