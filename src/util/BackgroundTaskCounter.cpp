#include "util/BackgroundTaskCounter.h"

#include <atomic>
#include <cstdio>

#include "util/Waker.h"

namespace microide::util {

namespace {

std::atomic<int> g_background_task_count{0};
std::atomic<WakeChannel> g_wake_channel{0};

}  // namespace

void SetBackgroundTaskWakeChannel(WakeChannel channel) {
  g_wake_channel.store(channel, std::memory_order_release);
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
  // assert would convert a now-recovered condition into a debug/ASAN abort — and
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

  // Wake the event loop so it can check the new idle state. Route through the checked
  // pusher so a rejected push latches the shared "wake owed" bit and the idle poll
  // schedules a fallback wait rather than leaving the shell on the stale full-idle hint.
  // An unset channel takes the same route: PushWake(0) is a no-op, and the loop's
  // scheduled poll is what rechecks. (It used to fall back to the bare SDL_EVENT_USER
  // base, which aliases the first registered custom event and mis-routes every
  // completion wake into that subsystem's handler.)
  PushWake(g_wake_channel.load(std::memory_order_acquire));
}

int GetBackgroundTaskCount() {
  return g_background_task_count.load(std::memory_order_acquire);
}

}  // namespace microide::util
