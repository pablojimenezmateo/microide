#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace microide::util {

class CancellationToken {
 public:
  CancellationToken() = default;

  bool IsCancellationRequested() const;

 private:
  struct State {
    std::atomic<bool> cancelled{false};
  };

  explicit CancellationToken(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;

  friend class TaskExecutor;
};

class TaskExecutor {
 public:
  using Task = std::function<void(const CancellationToken&)>;

  TaskExecutor();
  ~TaskExecutor();
  TaskExecutor(const TaskExecutor&) = delete;
  TaskExecutor& operator=(const TaskExecutor&) = delete;

  void Submit(Task task);
  void CancelAll();
  void WaitForIdle();

 private:
  struct TaskEntry {
    Task task;
    std::shared_ptr<CancellationToken::State> state;
  };

  void WorkerMain();
  void RequestShutdown();

  std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable idle_cv_;
  std::deque<TaskEntry> pending_;
  std::shared_ptr<CancellationToken::State> active_state_;
  std::thread worker_;
  bool shutdown_requested_ = false;
};

}  // namespace microide::util
