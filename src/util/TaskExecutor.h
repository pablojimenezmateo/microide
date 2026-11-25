#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

  explicit TaskExecutor(std::size_t thread_count = 1);
  ~TaskExecutor();
  TaskExecutor(const TaskExecutor&) = delete;
  TaskExecutor& operator=(const TaskExecutor&) = delete;

  void Submit(Task task);
  void Submit(std::string coalesce_key, Task task);
  void CancelAll();
  void WaitForIdle();

 private:
  void CancelAllLocked();
  bool IdleLocked() const;
  void NotifyIfIdleLocked();
  struct TaskEntry {
    Task task;
    std::shared_ptr<CancellationToken::State> state;
    std::string coalesce_key;
  };

  void WorkerMain(std::size_t slot);
  void RequestShutdown();

  std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable idle_cv_;
  std::deque<TaskEntry> pending_;
  std::vector<std::shared_ptr<CancellationToken::State>> active_states_;
  std::vector<std::thread> workers_;
  bool shutdown_requested_ = false;
};

}
