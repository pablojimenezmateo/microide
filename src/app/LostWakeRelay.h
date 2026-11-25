#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace microide::app {

class LostWakeRelay {
 public:
  using Nudge = std::function<void()>;

  LostWakeRelay(std::chrono::milliseconds delay, Nudge nudge);
  ~LostWakeRelay();

  LostWakeRelay(const LostWakeRelay&) = delete;
  LostWakeRelay& operator=(const LostWakeRelay&) = delete;

  void NotePush();
  void EnterBlockingWait();
  void LeaveBlockingWait();

  std::uint64_t nudges() const { return nudges_.load(std::memory_order_relaxed); }
  std::uint64_t pushes() const { return pushes_.load(std::memory_order_relaxed); }
  std::uint64_t first_push_in_wait_ms() const { return first_push_in_wait_ms_.load(std::memory_order_relaxed); }

 private:
  void Run();
  bool WakeLooksLost() const;

  const std::chrono::milliseconds delay_;
  const Nudge nudge_;
  std::atomic<std::uint64_t> pushes_{0};
  std::atomic<std::uint64_t> pushes_at_wait_entry_{0};
  std::atomic<bool> in_wait_{false};
  std::atomic<std::uint64_t> nudges_{0};
  std::atomic<std::uint64_t> first_push_in_wait_ms_{0};
  std::mutex mutex_;
  std::condition_variable wake_;
  bool armed_ = false;
  bool stop_ = false;
  std::thread thread_;
};

}
