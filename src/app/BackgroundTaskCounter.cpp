#include "app/BackgroundTaskCounter.h"

#include <SDL3/SDL.h>

#include <atomic>

namespace microide::app {

namespace {

std::atomic<int> g_background_task_count{0};
std::atomic<std::uint32_t> g_wake_event_type{0};

}  // namespace

void SetBackgroundTaskWakeEventType(std::uint32_t event_type) {
  g_wake_event_type.store(event_type, std::memory_order_release);
}

void IncrementBackgroundTaskCount() {
  g_background_task_count.fetch_add(1, std::memory_order_acq_rel);
}

void DecrementBackgroundTaskCountAndWake() {
  const int prev = g_background_task_count.fetch_sub(1, std::memory_order_acq_rel);
  SDL_assert(prev > 0 && "BackgroundTaskCounter underflow: more decrements than increments");

  // Wake the event loop so it can check the new idle state. Use the dedicated
  // registered event type when set; the bare SDL_EVENT_USER fallback aliases the
  // first registered custom event and mis-routes the wake into that subsystem's
  // handler on every task completion.
  const std::uint32_t wake_type = g_wake_event_type.load(std::memory_order_acquire);
  SDL_Event event{};
  event.type = wake_type != 0 ? wake_type : static_cast<std::uint32_t>(SDL_EVENT_USER);
  SDL_PushEvent(&event);
}

int GetBackgroundTaskCount() {
  return g_background_task_count.load(std::memory_order_acquire);
}

}  // namespace microide::app
