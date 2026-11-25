#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace microide::util {

class SerialWorkQueue {
 public:
  enum class StartMode { kLazy, kEager };

  enum class Shedding { kCritical, kDroppable };

  struct Limits {
    std::size_t max_depth = 0;
  };

  struct Hooks {
    std::function<void()> on_enqueue;
    std::function<void()> on_complete;
  };

  explicit SerialWorkQueue(StartMode start_mode = StartMode::kLazy, Hooks hooks = {},
                           Limits limits = Limits{0}, std::string thread_name = {});
  ~SerialWorkQueue();

  SerialWorkQueue(const SerialWorkQueue&) = delete;
  SerialWorkQueue& operator=(const SerialWorkQueue&) = delete;

  void EnsureStarted();
  bool started() const { return started_.load(std::memory_order_acquire); }

  void Post(std::function<void()> task, Shedding shedding = Shedding::kCritical);
  void PostLatest(std::string key, std::function<void()> task,
                  Shedding shedding = Shedding::kCritical);
  void PostFront(std::function<void()> task);
  void PostDurable(std::function<void()> task);

  void Cancel();

  void Drain();

  void Flush();

  void Shutdown(std::chrono::milliseconds deadline = std::chrono::milliseconds(2000));

 private:
  void WorkerMain();
  void EnsureStartedLocked();

  struct Job {
    std::string key;
    std::function<void()> task;
    bool cancelled = false;
    bool droppable = false;
    bool run_even_if_cancelled = false;
    bool durable = false;
  };

  using JobIter = std::list<Job>::iterator;

  void EraseQueuedLocked(JobIter it);
  void ShedIfOverBudgetLocked();
  void ClearKeyIndexLocked() { keyed_index_.clear(); }

  Hooks hooks_;
  Limits limits_;
  std::string thread_name_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::list<Job> queue_;
  std::unordered_map<std::string, JobIter> keyed_index_;
  std::thread worker_;
  std::atomic<bool> started_{false};
  bool stop_ = false;
};

}
