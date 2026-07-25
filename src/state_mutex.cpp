#include "state_mutex.h"

static StaticSemaphore_t s_mutexStorage;
static SemaphoreHandle_t s_mutex = nullptr;

void state_mutex_init() {
  s_mutex = xSemaphoreCreateMutexStatic(&s_mutexStorage);
}

void state_lock() {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
}

void state_unlock() {
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}
