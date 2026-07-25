#include "state_mutex.h"

static StaticSemaphore_t s_mutexStorage;
static SemaphoreHandle_t s_mutex = nullptr;

void state_mutex_init() {
  // Recursive, not a plain mutex -- several draw functions in
  // screen_manager.cpp call helper functions (e.g.
  // astro_recompute_moon_phase()) that also take this same lock
  // internally. Wrapping a whole draw function's body in state_lock()/
  // state_unlock() while it also calls one of those helpers would
  // deadlock on a standard mutex (a task can't re-acquire a mutex it
  // already holds). A recursive mutex allows the same task to safely
  // re-enter the lock any number of times, as long as lock/unlock calls
  // stay balanced.
  s_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_mutexStorage);
}

void state_lock() {
  if (s_mutex != nullptr) {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
  }
}

void state_unlock() {
  if (s_mutex != nullptr) {
    xSemaphoreGiveRecursive(s_mutex);
  }
}
