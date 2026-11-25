#pragma once

#include <chrono>
#include <functional>
#include <string>

#include "util/SerialWorkQueue.h"

namespace microide::project {

class ProjectBackgroundExecutor {
 public:
  ProjectBackgroundExecutor();
  ~ProjectBackgroundExecutor() = default;

  ProjectBackgroundExecutor(const ProjectBackgroundExecutor&) = delete;
  ProjectBackgroundExecutor& operator=(const ProjectBackgroundExecutor&) = delete;

  void Post(std::function<void()> task) { queue_.Post(std::move(task)); }

  void PostLatest(std::string key, std::function<void()> task) {
    queue_.PostLatest(std::move(key), std::move(task));
  }

  void PostDurable(std::function<void()> task) { queue_.PostDurable(std::move(task)); }

  void Cancel() { queue_.Cancel(); }

  void Drain() { queue_.Drain(); }

  void Shutdown(std::chrono::milliseconds deadline = std::chrono::milliseconds(2000)) {
    queue_.Shutdown(deadline);
  }

 private:
  util::SerialWorkQueue queue_;
};

}
