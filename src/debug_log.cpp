#include "debug_log.h"
#include <stdarg.h>

String g_debugLog[DEBUG_LOG_LINES];

void debug_log(const char* fmt, ...) {
  char buf[80];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  char line[96];
  snprintf(line, sizeof(line), "%lums %s", (unsigned long)millis(), buf);

  for (int i = 0; i < DEBUG_LOG_LINES - 1; i++) {
    g_debugLog[i] = g_debugLog[i + 1];
  }
  g_debugLog[DEBUG_LOG_LINES - 1] = String(line);

  Serial.println(line);
}
