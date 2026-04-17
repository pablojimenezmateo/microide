#include "util/TaskExecutor.h"

#include <utility>

namespace microide::util {

CancellationToken::CancellationToken(std::shared_ptr<State> state) : state_(std::move(state)) {}

bool CancellationToken::IsCancellationRequested() const {
  return state_ != nullptr && state_->cancelled.load();
}

TaskExecutor::TaskExecutor() : worker_(&TaskExecutor::WorkerMain, this) {}

TaskExecutor::~TaskExecutor() {
  RequestShutdown();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void TaskExecutor::Submit(Task task) {
  if (!task) {
    return;
  }

  auto state = std::make_shared<CancellationToken::State>();
  {
    std::lock_guard lock(mutex_);
    if (shutdown_requested_) {
      state->cancelled.store(true);
      return;
    }
    pending_.push_back(TaskEntry{
        .task = std::move(task),
        .state = std::move(state),
    });
  }
  cv_.notify_one();
}

void TaskExecutor::CancelAll() {
  std::lock_guard lock(mutex_);
  if (active_state_ != nullptr) {
    active_state_->cancelled.store(true);
  }
  for (auto& task : pending_) {
    if (task.state != nullptr) {
      task.state->cancelled.store(true);
    }
  }
  pending_.clear();
  if (active_state_ == nullptr) {
    idle_cv_.notify_all();
  }
}

void TaskExecutor::WaitForIdle() {
  std::unique_lock lock(mutex_);
  idle_cv_.wait(lock, [&]() { return pending_.empty() && active_state_ == nullptr; });
}

void TaskExecutor::WorkerMain() {
  while (true) {
    TaskEntry entry;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&]() { return shutdown_requested_ || !pending_.empty(); });
      if (shutdown_requested_ && pending_.empty()) {
        return;
      }

      entry = std::move(pending_.front());
      pending_.pop_front();
      active_state_ = entry.state;
    }

    CancellationToken token(entry.state);
    if (!token.IsCancellationRequested() && entry.task) {
      entry.task(token);
    }

    {
      std::lock_guard lock(mutex_);
      if (active_state_ == entry.state) {
        active_state_.reset();
      }
      if (pending_.empty() && active_state_ == nullptr) {
        idle_cv_.notify_all();
      }
    }
  }
}

void TaskExecutor::RequestShutdown() {
  {
    std::lock_guard lock(mutex_);
    shutdown_requested_ = true;
    if (active_state_ != nullptr) {
      active_state_->cancelled.store(true);
    }
    for (auto& task : pending_) {
      if (task.state != nullptr) {
        task.state->cancelled.store(true);
      }
    }
    pending_.clear();
    if (active_state_ == nullptr) {
      idle_cv_.notify_all();
    }
  }
  cv_.notify_all();
}

}  // namespace microide::util
