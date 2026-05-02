#include "app/BackgroundTaskCounter.h"

#include <SDL3/SDL.h>

#include <atomic>

namespace microide::app {

namespace {

std::atomic<int> g_background_task_count{0};

}  // namespace

void IncrementBackgroundTaskCount() {
  g_background_task_count.fetch_add(1, std::memory_order_acq_rel);
}

void DecrementBackgroundTaskCountAndWake() {
  const int prev = g_background_task_count.fetch_sub(1, std::memory_order_acq_rel);
  SDL_assert(prev > 0 && "BackgroundTaskCounter underflow: more decrements than increments");

  // Wake the event loop so it can check the new idle state.
  SDL_Event event{};
  event.type = SDL_EVENT_USER;
  SDL_PushEvent(&event);
}

int GetBackgroundTaskCount() {
  return g_background_task_count.load(std::memory_order_acquire);
}

}  // namespace microide::app
