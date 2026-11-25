#include "util/SerialWorkQueue.h"
#include "util/ThreadName.h"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <future>
#include <utility>
#include "util/InvokeIfSet.h"

namespace microide::util {

SerialWorkQueue::SerialWorkQueue(StartMode start_mode, Hooks hooks, Limits limits, std::string thread_name)
    : hooks_(std::move(hooks)), limits_(limits), thread_name_(std::move(thread_name)) {
  if (start_mode == StartMode::kEager) {
    EnsureStarted();
  }
}

SerialWorkQueue::~SerialWorkQueue() { Shutdown(); }

void SerialWorkQueue::EnsureStarted() {
  std::lock_guard lock(mutex_);
  EnsureStartedLocked();
}

void SerialWorkQueue::EnsureStartedLocked() {
  if (started_.load(std::memory_order_acquire) || stop_) {
    return;
  }
  worker_ = std::thread([this]() {
    if (!thread_name_.empty()) {
      NameCurrentThread(thread_name_);
    }
    WorkerMain();
  });
  started_.store(true, std::memory_order_release);
}

void SerialWorkQueue::EraseQueuedLocked(JobIter it) {
  if (!it->key.empty()) {
    if (const auto mit = keyed_index_.find(it->key);
        mit != keyed_index_.end() && mit->second == it) {
      keyed_index_.erase(mit);
    }
  }
  queue_.erase(it);
  util::InvokeIfSet(hooks_.on_complete);
}

void SerialWorkQueue::ShedIfOverBudgetLocked() {
  if (limits_.max_depth == 0 || queue_.size() < limits_.max_depth) {
    return;
  }
  for (auto it = queue_.begin(); it != queue_.end(); ++it) {
    if (it->droppable && !it->cancelled) {
      EraseQueuedLocked(it);
      return;
    }
  }
}

void SerialWorkQueue::Post(std::function<void()> task, Shedding shedding) {
  {
    std::lock_guard lock(mutex_);
    if (stop_) {
      return;
    }
    EnsureStartedLocked();
    ShedIfOverBudgetLocked();
    util::InvokeIfSet(hooks_.on_enqueue);
    queue_.push_back(
        Job{.key = {}, .task = std::move(task), .droppable = shedding == Shedding::kDroppable});
  }
  cv_.notify_one();
}

void SerialWorkQueue::PostDurable(std::function<void()> task) {
  {
    std::lock_guard lock(mutex_);
    if (stop_) {
      return;
    }
    EnsureStartedLocked();
    util::InvokeIfSet(hooks_.on_enqueue);
    queue_.push_back(Job{.key = {}, .task = std::move(task), .durable = true});
  }
  cv_.notify_one();
}

void SerialWorkQueue::PostFront(std::function<void()> task) {
  {
    std::lock_guard lock(mutex_);
    if (stop_) {
      return;
    }
    EnsureStartedLocked();
    ShedIfOverBudgetLocked();
    util::InvokeIfSet(hooks_.on_enqueue);
    queue_.push_front(Job{.key = {}, .task = std::move(task)});
  }
  cv_.notify_one();
}

void SerialWorkQueue::PostLatest(std::string key, std::function<void()> task, Shedding shedding) {
  {
    std::lock_guard lock(mutex_);
    if (stop_) {
      return;
    }
    EnsureStartedLocked();
    auto [map_it, inserted] = keyed_index_.try_emplace(key);
    if (!inserted) {
      queue_.erase(map_it->second);
      map_it->second = queue_.end();
      util::InvokeIfSet(hooks_.on_complete);
    }
    ShedIfOverBudgetLocked();
    util::InvokeIfSet(hooks_.on_enqueue);
    queue_.push_back(Job{.key = std::move(key),
                         .task = std::move(task),
                         .droppable = shedding == Shedding::kDroppable});
    map_it->second = std::prev(queue_.end());
  }
  cv_.notify_one();
}

void SerialWorkQueue::Cancel() {
  std::lock_guard lock(mutex_);
  for (auto& job : queue_) {
    job.cancelled = job.cancelled || !job.durable;
  }
  ClearKeyIndexLocked();
}

void SerialWorkQueue::Drain() {
  std::promise<void> barrier;
  std::future<void> drained = barrier.get_future();
  {
    std::lock_guard lock(mutex_);
    if (stop_ || !started_.load(std::memory_order_acquire)) {
      if (hooks_.on_complete) {
        for (std::size_t i = queue_.size(); i > 0; --i) {
          hooks_.on_complete();
        }
      }
      queue_.clear();
      ClearKeyIndexLocked();
      return;
    }
    for (auto& job : queue_) {
      job.cancelled = true;
    }
    ClearKeyIndexLocked();
    util::InvokeIfSet(hooks_.on_enqueue);
    queue_.push_back(Job{.key = {},
                         .task = [&barrier]() { barrier.set_value(); },
                         .cancelled = false,
                         .run_even_if_cancelled = true});
  }
  cv_.notify_one();
  drained.wait();
}

void SerialWorkQueue::Flush() {
  std::promise<void> barrier;
  std::future<void> flushed = barrier.get_future();
  {
    std::lock_guard lock(mutex_);
    if (stop_ || !started_.load(std::memory_order_acquire)) {
      return;
    }
    util::InvokeIfSet(hooks_.on_enqueue);
    queue_.push_back(Job{.key = {},
                         .task = [&barrier]() { barrier.set_value(); },
                         .cancelled = false,
                         .run_even_if_cancelled = true});
  }
  cv_.notify_one();
  flushed.wait();
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
    ClearKeyIndexLocked();
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    (void)deadline;
    worker_.join();
  } else if (hooks_.on_complete) {
    std::lock_guard lock(mutex_);
    for (std::size_t i = queue_.size(); i > 0; --i) {
      hooks_.on_complete();
    }
    queue_.clear();
    ClearKeyIndexLocked();
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
      const JobIter front = queue_.begin();
      if (!front->key.empty()) {
        if (const auto mit = keyed_index_.find(front->key);
            mit != keyed_index_.end() && mit->second == front) {
          keyed_index_.erase(mit);
        }
      }
      job = std::move(*front);
      queue_.pop_front();
    }
    if (job.task && (!job.cancelled || job.run_even_if_cancelled)) {
      try {
        job.task();
      } catch (const std::exception& ex) {
        std::fprintf(stderr, "[work-queue] job threw std::exception: %s\n", ex.what());
      } catch (...) {
        std::fprintf(stderr, "[work-queue] job threw a non-standard exception\n");
      }
    }
    util::InvokeIfSet(hooks_.on_complete);
  }
}

}
