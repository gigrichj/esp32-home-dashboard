#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Single global mutex protecting every piece of shared application state
// (weather, aircraft, ISS, astro forecast, SpaceX launches, air quality,
// trend history, etc.) that gets written by the network task (Core 0)
// and read by the UI task (Core 1) while drawing.
//
// Neither side holds this across network I/O: fetch functions parse
// HTTP responses into local variables first (the slow part, seconds),
// then take the lock only for the brief final copy into the shared
// globals. Draw functions hold it for the duration of drawing one page
// (fast, no I/O). This closes a data-race window that was a strong
// candidate for this project's long-standing intermittent display
// glitch -- previously the UI task could read a global mid-write by the
// network task, producing torn/garbled data that got rendered as-is.
void state_mutex_init();
void state_lock();
void state_unlock();
