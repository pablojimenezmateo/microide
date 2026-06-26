#pragma once

#include <chrono>
#include <functional>
#include <string>

#include "util/SerialWorkQueue.h"

namespace microide::project {

// Single-thread background executor per project. A thin wrapper over
// util::SerialWorkQueue that wires the global BackgroundTaskCounter (so in-flight
// background work feeds the idle/wake policy) and starts its worker eagerly.
class ProjectBackgroundExecutor {
 public:
  ProjectBackgroundExecutor();
  ~ProjectBackgroundExecutor() = default;

  ProjectBackgroundExecutor(const ProjectBackgroundExecutor&) = delete;
  ProjectBackgroundExecutor& operator=(const ProjectBackgroundExecutor&) = delete;

  // Post a task to the back of the queue. Returns immediately.
  void Post(std::function<void()> task) { queue_.Post(std::move(task)); }

  // Post a task, replacing any previously queued (not yet running) task with the
  // same key. Used for blame debounce: rapid triggers discard superseded requests.
  void PostLatest(std::string key, std::function<void()> task) {
    queue_.PostLatest(std::move(key), std::move(task));
  }

  // Mark all queued (not in-flight) tasks as cancelled. Does not wait for the
  // in-flight task.
  void Cancel() { queue_.Cancel(); }

  // Cancel + shut down the worker thread, waiting up to deadline.
  void Shutdown(std::chrono::milliseconds deadline = std::chrono::milliseconds(2000)) {
    queue_.Shutdown(deadline);
  }

 private:
  util::SerialWorkQueue queue_;
};

}  // namespace microide::project
