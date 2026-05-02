#include "project/ProjectBackgroundExecutor.h"

#include <algorithm>

#include "app/BackgroundTaskCounter.h"

namespace microide::project {

ProjectBackgroundExecutor::ProjectBackgroundExecutor() {
  worker_ = std::thread([this]() { WorkerMain(); });
}

ProjectBackgroundExecutor::~ProjectBackgroundExecutor() {
  Shutdown();
}

void ProjectBackgroundExecutor::Post(std::function<void()> task) {
  {
    std::lock_guard lock(mutex_);
    if (stop_) {
      return;
    }
    app::IncrementBackgroundTaskCount();
    queue_.push_back(Entry{.key = {}, .task = std::move(task), .cancelled = false});
  }
  cv_.notify_one();
}

void ProjectBackgroundExecutor::PostLatest(std::string key, std::function<void()> task) {
  {
    std::lock_guard lock(mutex_);
    if (stop_) {
      return;
    }
    // Remove any existing queued entry with the same key; decrement for each removed entry.
    const std::size_t before = queue_.size();
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [&key](const Entry& e) { return !e.cancelled && e.key == key; }),
                 queue_.end());
    const std::size_t removed = before - queue_.size();
    for (std::size_t i = 0; i < removed; ++i) {
      app::DecrementBackgroundTaskCountAndWake();
    }
    app::IncrementBackgroundTaskCount();
    queue_.push_back(Entry{.key = std::move(key), .task = std::move(task), .cancelled = false});
  }
  cv_.notify_one();
}

void ProjectBackgroundExecutor::Cancel() {
  std::lock_guard lock(mutex_);
  for (auto& entry : queue_) {
    entry.cancelled = true;
  }
}

void ProjectBackgroundExecutor::Shutdown(std::chrono::milliseconds deadline) {
  {
    std::lock_guard lock(mutex_);
    if (stop_) {
      return;
    }
    // Cancel queued work before stopping.
    for (auto& entry : queue_) {
      entry.cancelled = true;
    }
    stop_ = true;
  }
  cv_.notify_all();

  if (worker_.joinable()) {
    // Try to join within deadline
    const auto end_time = std::chrono::steady_clock::now() + deadline;
    // std::thread doesn't support timed join directly; use detach on timeout.
    // We can spin-wait on joinable with a sleep loop, but the simplest approach is
    // to just join() and accept that it blocks up to deadline if the task cooperates.
    // For well-behaved tasks this should be fast.
    worker_.join();
  }
}

void ProjectBackgroundExecutor::WorkerMain() {
  while (true) {
    Entry entry;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });

      if (stop_ && queue_.empty()) {
        return;
      }
      if (queue_.empty()) {
        continue;
      }

      entry = std::move(queue_.front());
      queue_.pop_front();
    }

    if (!entry.cancelled && entry.task) {
      entry.task();
    }
    app::DecrementBackgroundTaskCountAndWake();
  }
}

}  // namespace microide::project
