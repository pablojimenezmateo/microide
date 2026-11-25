#include "util/BackgroundTaskCounter.h"

#include <atomic>
#include <cstdio>

#include "util/Waker.h"

namespace microide::util {

namespace {

std::atomic<int> g_background_task_count{0};
std::atomic<WakeChannel> g_wake_channel{0};

}

void SetBackgroundTaskWakeChannel(WakeChannel channel) {
  g_wake_channel.store(channel, std::memory_order_release);
}

void IncrementBackgroundTaskCount() {
  g_background_task_count.fetch_add(1, std::memory_order_acq_rel);
}

void DecrementBackgroundTaskCountAndWake() {
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

  PushWake(g_wake_channel.load(std::memory_order_acquire));
}

int GetBackgroundTaskCount() {
  return g_background_task_count.load(std::memory_order_acquire);
}

}
