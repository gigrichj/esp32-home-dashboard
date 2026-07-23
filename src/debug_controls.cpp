#include "debug_controls.h"
#include <esp_system.h>

uint32_t g_aviationPollMs = 15000;

const char* g_resetReasonStr = "unknown";
size_t g_minFreeHeapSeen = SIZE_MAX;

static const char* resetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "POWER-ON";
    case ESP_RST_EXT:       return "EXTERNAL PIN";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC (crash)";
    case ESP_RST_INT_WDT:   return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT:  return "TASK WATCHDOG";
    case ESP_RST_WDT:       return "OTHER WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEP SLEEP WAKE";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (power)";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

void debug_controls_record_reset_reason() {
  g_resetReasonStr = resetReasonToString(esp_reset_reason());
}

void debug_controls_update_min_heap() {
  size_t freeHeap = esp_get_free_heap_size();
  if (freeHeap < g_minFreeHeapSeen) {
    g_minFreeHeapSeen = freeHeap;
  }
}

void cycleAviationPollInterval() {
  g_aviationPollMs += 15000;
  if (g_aviationPollMs > 60000) {
    g_aviationPollMs = 15000;
  }
}
