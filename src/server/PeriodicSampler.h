#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "util/ThreadName.h"

namespace microide::server {

// Runs `sample` every `interval` on its own thread until destroyed (which joins
// it, so `sample` may use whatever outlives the sampler). For host state nothing
// notifies about: a repository moved by a commit outside the server, a port a
// host program started listening on.
class PeriodicSampler {
 public:
  // `name`: the thread's (util::NameCurrentThread).
  PeriodicSampler(std::string_view name, std::chrono::milliseconds interval, std::function<void()> sample)
      : sample_(std::move(sample)) {
    thread_ = std::thread([this, interval, name = std::string(name)]() {
      util::NameCurrentThread(name);
      Run(interval);
    });
  }
  ~PeriodicSampler() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  PeriodicSampler(const PeriodicSampler&) = delete;
  PeriodicSampler& operator=(const PeriodicSampler&) = delete;

 private:
  void Run(std::chrono::milliseconds interval) {
    std::unique_lock lock(mutex_);
    while (!cv_.wait_for(lock, interval, [this] { return stop_; })) {
      lock.unlock();
      sample_();
      lock.lock();
    }
  }

  std::function<void()> sample_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::thread thread_;  // last
};

}  // namespace microide::server
