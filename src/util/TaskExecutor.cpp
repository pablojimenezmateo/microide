#include "util/TaskExecutor.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <utility>

#include "util/PerformanceCounters.h"

namespace microide::util {

CancellationToken::CancellationToken(std::shared_ptr<State> state) : state_(std::move(state)) {}

bool CancellationToken::IsCancellationRequested() const {
  return state_ != nullptr && state_->cancelled.load(std::memory_order_relaxed);
}

TaskExecutor::TaskExecutor(std::size_t thread_count) {
  const std::size_t n = thread_count > 0 ? thread_count : 1;
  active_states_.resize(n);
  workers_.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    workers_.emplace_back(&TaskExecutor::WorkerMain, this, i);
  }
}

TaskExecutor::~TaskExecutor() {
  RequestShutdown();
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
}

void TaskExecutor::Submit(Task task) { Submit(std::string{}, std::move(task)); }

void TaskExecutor::Submit(std::string coalesce_key, Task task) {
  if (!task) return;
  auto state = std::make_shared<CancellationToken::State>();
  {
    std::lock_guard lock(mutex_);
    if (shutdown_requested_) {
      state->cancelled.store(true);
      return;
    }
    if (!coalesce_key.empty()) {
      // Drop still-queued work for the same subject: cancel its token (so a racing
      // reader sees it obsolete) and remove it from the deque so the worker never
      // wakes for it. Only queued entries live in pending_; an entry already
      // popped into an active slot is intentionally untouched.
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->coalesce_key == coalesce_key) {
          if (it->state != nullptr) it->state->cancelled.store(true);
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
    }
    // Counted before the worker can coalesce or cancel it, so
    // enqueued-minus-run is the amount of submitted work that never ran. That
    // gap is the signal; treating them as equal would hide it.
    AddPerformanceCounter(PerfCounterId::TaskExecutorTasksEnqueued);
    pending_.push_back(TaskEntry{.task = std::move(task),
                                 .state = std::move(state),
                                 .coalesce_key = std::move(coalesce_key)});
  }
  cv_.notify_one();
}

void TaskExecutor::CancelAll() {
  std::lock_guard lock(mutex_);
  for (auto& s : active_states_) {
    if (s != nullptr) s->cancelled.store(true);
  }
  for (auto& task : pending_) {
    if (task.state != nullptr) task.state->cancelled.store(true);
  }
  pending_.clear();
  const bool all_idle = std::all_of(active_states_.begin(), active_states_.end(),
                                    [](const auto& s) { return s == nullptr; });
  if (all_idle) idle_cv_.notify_all();
}

void TaskExecutor::WaitForIdle() {
  std::unique_lock lock(mutex_);
  idle_cv_.wait(lock, [&]() {
    if (!pending_.empty()) return false;
    return std::all_of(active_states_.begin(), active_states_.end(),
                       [](const auto& s) { return s == nullptr; });
  });
}

void TaskExecutor::WorkerMain(std::size_t slot) {
  while (true) {
    TaskEntry entry;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&]() { return shutdown_requested_ || !pending_.empty(); });
      if (shutdown_requested_ && pending_.empty()) return;

      entry = std::move(pending_.front());
      pending_.pop_front();
      active_states_[slot] = entry.state;
    }

    CancellationToken token(entry.state);
    if (!token.IsCancellationRequested() && entry.task) {
      // Exception firewall: background tasks touch the filesystem, allocation-heavy
      // vectors, and regex/search state. An uncaught std::filesystem_error/bad_alloc
      // would escape the std::thread entry point and std::terminate the whole IDE —
      // and skip the active_states_ clear below, hanging WaitForIdle(). Swallow it
      // here and keep the worker alive, matching SerialWorkQueue's firewall.
      try {
        AddPerformanceCounter(PerfCounterId::TaskExecutorTasksRun);
        entry.task(token);
      } catch (const std::exception& ex) {
        std::fprintf(stderr, "[task-executor] task threw std::exception: %s\n", ex.what());
      } catch (...) {
        std::fprintf(stderr, "[task-executor] task threw a non-standard exception\n");
      }
    }

    {
      std::lock_guard lock(mutex_);
      if (active_states_[slot] == entry.state) {
        active_states_[slot].reset();
      }
      const bool all_idle = pending_.empty() &&
          std::all_of(active_states_.begin(), active_states_.end(),
                      [](const auto& s) { return s == nullptr; });
      if (all_idle) idle_cv_.notify_all();
    }
  }
}

void TaskExecutor::RequestShutdown() {
  {
    std::lock_guard lock(mutex_);
    shutdown_requested_ = true;
    for (auto& s : active_states_) {
      if (s != nullptr) s->cancelled.store(true);
    }
    for (auto& task : pending_) {
      if (task.state != nullptr) task.state->cancelled.store(true);
    }
    pending_.clear();
    const bool all_idle = std::all_of(active_states_.begin(), active_states_.end(),
                                      [](const auto& s) { return s == nullptr; });
    if (all_idle) idle_cv_.notify_all();
  }
  cv_.notify_all();
}

}  // namespace microide::util
