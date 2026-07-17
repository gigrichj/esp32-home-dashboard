#include "debug_log.h"
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

String g_debugLog[DEBUG_LOG_LINES];

// debug_log() is called from both networkTask (Core 0) and the display
// task (Core 1, for refresh-timeout logging), so the shared ring buffer
// needs real mutual exclusion - without it, concurrent writes can corrupt
// or silently drop entries, which is exactly what we saw (an out-of-order
// timestamp in the captured log).
static StaticSemaphore_t debugMutexStorage;
static SemaphoreHandle_t debugMutex = xSemaphoreCreateMutexStatic(&debugMutexStorage);

void debug_log(const char* fmt, ...) {
  char buf[80];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  char line[96];
  snprintf(line, sizeof(line), "%lums %s", (unsigned long)millis(), buf);

  if (xSemaphoreTake(debugMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (int i = 0; i < DEBUG_LOG_LINES - 1; i++) {
      g_debugLog[i] = g_debugLog[i + 1];
    }
    g_debugLog[DEBUG_LOG_LINES - 1] = String(line);
    xSemaphoreGive(debugMutex);
  }

  Serial.println(line);
}
