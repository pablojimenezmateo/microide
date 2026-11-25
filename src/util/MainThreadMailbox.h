#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "util/Waker.h"

namespace microide::util {

class MainThreadMailbox {
 public:
  using Action = std::function<void()>;

  MainThreadMailbox() = default;
  MainThreadMailbox(const MainThreadMailbox&) = delete;
  MainThreadMailbox& operator=(const MainThreadMailbox&) = delete;

  void SetWakeChannel(WakeChannel channel);

  void Post(Action action);

  void PostWithoutWake(Action action);

  void PostLatest(std::string key, Action action);

  bool PushWake() const;

  int Drain();

  void Clear();

  int PendingCount() const { return queued_.load(std::memory_order_acquire); }

  bool HasUndeliveredWake() const {
    return wake_delivery_failed_.load(std::memory_order_acquire);
  }

  bool RetryWakeIfPending() const;

 private:
  std::mutex mutex_;
  std::vector<Action> actions_;
  std::unordered_map<std::string, std::size_t> keyed_index_;
  std::atomic<int> queued_{0};
  std::atomic<WakeChannel> wake_channel_{0};
  mutable std::atomic<bool> wake_delivery_failed_{false};
};

}
