#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace microide::project {

// Single-thread background executor per project.
// Supports basic task queuing, dedup-by-key (PostLatest), cancellation, and shutdown.
class ProjectBackgroundExecutor {
 public:
  ProjectBackgroundExecutor();
  ~ProjectBackgroundExecutor();

  ProjectBackgroundExecutor(const ProjectBackgroundExecutor&) = delete;
  ProjectBackgroundExecutor& operator=(const ProjectBackgroundExecutor&) = delete;

  // Post a task to the back of the queue. Returns immediately.
  void Post(std::function<void()> task);

  // Post a task, replacing any previously queued (not yet running) task with the same key.
  // Used for blame debounce: rapid triggers discard superseded requests.
  void PostLatest(std::string key, std::function<void()> task);

  // Mark all queued (not in-flight) tasks as cancelled. Does not wait for in-flight task.
  void Cancel();

  // Cancel + shut down the worker thread, waiting up to deadline.
  void Shutdown(std::chrono::milliseconds deadline = std::chrono::milliseconds(2000));

 private:
  void WorkerMain();

  struct Entry {
    std::string key;  // empty = no dedup
    std::function<void()> task;
    bool cancelled = false;
  };

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Entry> queue_;
  std::thread worker_;
  bool stop_ = false;
};

}  // namespace microide::project
