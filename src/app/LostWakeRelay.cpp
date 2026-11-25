#include "app/LostWakeRelay.h"

#include <utility>

#include "util/ThreadName.h"

namespace microide::app {

LostWakeRelay::LostWakeRelay(std::chrono::milliseconds delay, Nudge nudge)
    : delay_(delay), nudge_(std::move(nudge)), thread_([this] { Run(); }) {}

LostWakeRelay::~LostWakeRelay() {
  {
    std::scoped_lock lock(mutex_);
    stop_ = true;
  }
  wake_.notify_one();
  thread_.join();
}

void LostWakeRelay::NotePush() {
  pushes_.fetch_add(1, std::memory_order_seq_cst);
  if (!in_wait_.load(std::memory_order_seq_cst)) {
    return;
  }
  std::uint64_t none = 0;
  (void)first_push_in_wait_ms_.compare_exchange_strong(
      none, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count()),
      std::memory_order_relaxed);
  {
    std::scoped_lock lock(mutex_);
    armed_ = true;
  }
  wake_.notify_one();
}

void LostWakeRelay::EnterBlockingWait() {
  first_push_in_wait_ms_.store(0, std::memory_order_relaxed);
  pushes_at_wait_entry_.store(pushes_.load(std::memory_order_seq_cst), std::memory_order_seq_cst);
  in_wait_.store(true, std::memory_order_seq_cst);
}

void LostWakeRelay::LeaveBlockingWait() { in_wait_.store(false, std::memory_order_seq_cst); }

bool LostWakeRelay::WakeLooksLost() const {
  return in_wait_.load(std::memory_order_seq_cst) &&
         pushes_.load(std::memory_order_seq_cst) >
             pushes_at_wait_entry_.load(std::memory_order_seq_cst);
}

void LostWakeRelay::Run() {
  util::NameCurrentThread("wake-relay");
  std::unique_lock lock(mutex_);
  while (true) {
    wake_.wait(lock, [this] { return armed_ || stop_; });
    if (stop_) {
      return;
    }
    armed_ = false;
    if (wake_.wait_for(lock, delay_, [this] { return stop_; })) {
      return;
    }
    if (WakeLooksLost()) {
      nudges_.fetch_add(1, std::memory_order_relaxed);
      lock.unlock();
      nudge_();
      lock.lock();
    }
  }
}

}
