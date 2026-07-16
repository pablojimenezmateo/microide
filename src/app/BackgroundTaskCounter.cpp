#include "app/BackgroundTaskCounter.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>

#include "util/SdlWake.h"

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
  // Saturating decrement: never push the count below zero. A blind fetch_sub goes
  // negative on an unmatched decrement (double-completion or a decrement on a path
  // that never incremented), and the next real task then increments -1 -> 0, so
  // GetBackgroundTaskCount() == 0 hides genuine in-flight work and the event loop
  // treats it as idle. The CAS loop leaves the count at zero on underflow. An underflow
  // is logged (not asserted): the clamp already HANDLES it gracefully, so a hard
  // SDL_assert would convert a now-recovered condition into a debug/ASAN abort — and
  // would fire on the legitimate underflow-clamp regression test. Log-and-continue
  // matches SerialWorkQueue's firewall.
  int current = g_background_task_count.load(std::memory_order_acquire);
  bool underflow = false;
  while (true) {
    if (current <= 0) {
      underflow = true;
      break;
    }
    if (g_background_task_count.compare_exchange_weak(current, current - 1,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
      break;
    }
  }
  if (underflow) {
    std::fprintf(stderr,
                 "[background-task-counter] unmatched decrement (more decrements than "
                 "increments); clamped at zero\n");
  }

  // Wake the event loop so it can check the new idle state. Use the dedicated
  // registered event type when set; the bare SDL_EVENT_USER fallback aliases the
  // first registered custom event and mis-routes the wake into that subsystem's
  // handler on every task completion. Route through the checked pusher so a rejected
  // push latches the shared "wake owed" bit and the idle poll schedules a fallback
  // wait rather than leaving the shell on the stale full-idle hint.
  const std::uint32_t wake_type = g_wake_event_type.load(std::memory_order_acquire);
  util::PushSdlWake(wake_type != 0 ? wake_type : static_cast<std::uint32_t>(SDL_EVENT_USER));
}

int GetBackgroundTaskCount() {
  return g_background_task_count.load(std::memory_order_acquire);
}

}  // namespace microide::app
