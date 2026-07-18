#include "debug_controls.h"

uint32_t g_aviationPollMs = 15000;

void cycleAviationPollInterval() {
  g_aviationPollMs += 15000;
  if (g_aviationPollMs > 60000) {
    g_aviationPollMs = 15000;
  }
}
