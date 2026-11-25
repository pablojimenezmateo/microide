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

class PeriodicSampler {
 public:
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
  std::thread thread_;
};

}
