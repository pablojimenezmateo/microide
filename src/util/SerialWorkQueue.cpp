#include "util/SerialWorkQueue.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace microide::util {

SerialWorkQueue::SerialWorkQueue(StartMode start_mode, Hooks hooks) : hooks_(std::move(hooks)) {
  if (start_mode == StartMode::kEager) {
    EnsureStarted();
  }
}

SerialWorkQueue::~SerialWorkQueue() { Shutdown(); }

void SerialWorkQueue::EnsureStarted() {
  std::lock_guard lock(mutex_);
  if (started_.load(std::memory_order_acquire) || stop_) {
    return;
  }
  worker_ = std::thread([this]() { WorkerMain(); });
  started_.store(true, std::memory_order_release);
}

void SerialWorkQueue::Post(std::function<void()> task) {
  {
    std::lock_guard lock(mutex_);
    if (stop_) {
      return;
    }
    if (hooks_.on_enqueue) {
      hooks_.on_enqueue();
    }
    queue_.push_back(Job{.key = {}, .task = std::move(task), .cancelled = false});
  }
  cv_.notify_one();
}

void SerialWorkQueue::PostFront(std::function<void()> task) {
  {
    std::lock_guard lock(mutex_);
    if (stop_) {
      return;
    }
    if (hooks_.on_enqueue) {
      hooks_.on_enqueue();
    }
    queue_.push_front(Job{.key = {}, .task = std::move(task), .cancelled = false});
  }
  cv_.notify_one();
}

void SerialWorkQueue::PostLatest(std::string key, std::function<void()> task) {
  {
    std::lock_guard lock(mutex_);
    if (stop_) {
      return;
    }
    // Drop superseded (queued, not cancelled, same key) jobs and balance their
    // enqueue hooks so the in-flight accounting stays exact.
    const std::size_t before = queue_.size();
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [&key](const Job& job) {
                                  return !job.cancelled && job.key == key;
                                }),
                 queue_.end());
    if (hooks_.on_complete) {
      for (std::size_t i = before - queue_.size(); i > 0; --i) {
        hooks_.on_complete();
      }
    }
    if (hooks_.on_enqueue) {
      hooks_.on_enqueue();
    }
    queue_.push_back(Job{.key = std::move(key), .task = std::move(task), .cancelled = false});
  }
  cv_.notify_one();
}

void SerialWorkQueue::Cancel() {
  std::lock_guard lock(mutex_);
  for (auto& job : queue_) {
    job.cancelled = true;
  }
}

void SerialWorkQueue::Shutdown(std::chrono::milliseconds deadline) {
  {
    std::lock_guard lock(mutex_);
    if (stop_) {
      return;
    }
    for (auto& job : queue_) {
      job.cancelled = true;
    }
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    // std::thread has no timed join; in-flight jobs are kept bounded by callers so
    // this blocking join stays prompt. The deadline is reserved for a future
    // cooperative-cancellation path.
    (void)deadline;
    worker_.join();
  } else if (hooks_.on_complete) {
    // Never-started queue: no worker will drain these jobs, so balance their
    // enqueue hooks here. (Production users with hooks start eagerly, so this is
    // a defensive path that keeps the on_complete contract unconditional.)
    std::lock_guard lock(mutex_);
    for (std::size_t i = queue_.size(); i > 0; --i) {
      hooks_.on_complete();
    }
    queue_.clear();
  }
}

void SerialWorkQueue::WorkerMain() {
  while (true) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_ && queue_.empty()) {
        return;
      }
      if (queue_.empty()) {
        continue;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    if (!job.cancelled && job.task) {
      job.task();
    }
    if (hooks_.on_complete) {
      hooks_.on_complete();
    }
  }
}

}  // namespace microide::util
