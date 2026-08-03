#include "util/SerialWorkQueue.h"

#include <cstddef>
#include <cstdio>
#include <exception>
#include <future>
#include <utility>

namespace microide::util {

SerialWorkQueue::SerialWorkQueue(StartMode start_mode, Hooks hooks, Limits limits)
    : hooks_(std::move(hooks)), limits_(limits) {
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
  worker_ = std::thread([this]() { WorkerMain(); });
  started_.store(true, std::memory_order_release);
}

void SerialWorkQueue::EraseQueuedLocked(JobIter it) {
  if (!it->key.empty()) {
    // Only drop the index entry when it still names THIS node — a later PostLatest
    // (e.g. after a Cancel cleared the index) may have re-pointed the key at a
    // newer node that must survive.
    if (const auto mit = keyed_index_.find(it->key);
        mit != keyed_index_.end() && mit->second == it) {
      keyed_index_.erase(mit);
    }
  }
  queue_.erase(it);
  if (hooks_.on_complete) {
    hooks_.on_complete();
  }
}

void SerialWorkQueue::ShedIfOverBudgetLocked() {
  if (limits_.max_depth == 0 || queue_.size() < limits_.max_depth) {
    return;
  }
  // Shed the oldest droppable, still-live job. Critical jobs are never shed, so a
  // queue saturated with critical work simply exceeds the budget rather than
  // losing correctness-bearing work.
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
    // Honor the lazy-start contract: a kLazy queue starts its worker on first post so
    // a caller can default-construct + Post without a separate EnsureStarted() and
    // still have the job run (rather than sit forever in an unstarted queue).
    EnsureStartedLocked();
    ShedIfOverBudgetLocked();
    if (hooks_.on_enqueue) {
      hooks_.on_enqueue();
    }
    queue_.push_back(
        Job{.key = {}, .task = std::move(task), .droppable = shedding == Shedding::kDroppable});
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
    if (hooks_.on_enqueue) {
      hooks_.on_enqueue();
    }
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
    // O(1) dedup: find (or reserve) the key's slot. If a live job with this key is
    // already queued, drop it (balancing its on_complete) so only the latest work
    // for this key runs; the replacement is appended at the tail below.
    auto [map_it, inserted] = keyed_index_.try_emplace(key);
    if (!inserted) {
      queue_.erase(map_it->second);
      // Retarget the slot immediately: ShedIfOverBudgetLocked below walks the queue
      // and compares each node against the index entry for its key, and comparing a
      // just-erased list iterator is undefined. end() is a stable, comparable value
      // that matches no live node, and the real node is stored a few lines down.
      map_it->second = queue_.end();
      if (hooks_.on_complete) {
        hooks_.on_complete();
      }
    }
    ShedIfOverBudgetLocked();
    if (hooks_.on_enqueue) {
      hooks_.on_enqueue();
    }
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
    job.cancelled = true;
  }
  // Every queued node is now cancelled, so none is a live dedup target.
  ClearKeyIndexLocked();
}

void SerialWorkQueue::Drain() {
  std::promise<void> barrier;
  std::future<void> drained = barrier.get_future();
  {
    std::lock_guard lock(mutex_);
    if (stop_ || !started_.load(std::memory_order_acquire)) {
      // Never started (no worker will ever run the barrier) or already stopped:
      // there is no in-flight job to wait for. Cancel any queued jobs for symmetry
      // and balance their enqueue hooks so the counter stays exact.
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
    // Trailing, non-cancelled barrier: it runs after the current in-flight job and
    // all the now-skipped queued jobs, so its completion means the worker is idle.
    if (hooks_.on_enqueue) {
      hooks_.on_enqueue();
    }
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
      // Never started: nothing was ever queued, so there is nothing to complete.
      // Already stopped: Shutdown() cancelled the backlog and joined the worker, so
      // no one is left to run a barrier -- blocking on one would hang forever.
      return;
    }
    // Unlike Drain(), leave the queued jobs alone: the barrier simply goes to the
    // TAIL, so waiting on it waits for all of them to run first.
    if (hooks_.on_enqueue) {
      hooks_.on_enqueue();
    }
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
    // std::thread has no timed join; in-flight jobs are kept bounded by callers so
    // this blocking join stays prompt. The deadline is reserved for a future
    // cooperative-cancellation path.
    (void)deadline;
    worker_.join();
  } else if (hooks_.on_complete) {
    // Never-started queue: no worker will drain these jobs, so balance their
    // enqueue hooks here. (Production users with hooks start eagerly, so this is
    // a defensive path that keeps the on_complete contract unconditional.)
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
      // Drop the front node's key-index entry before popping it (only if the index
      // still names this exact node — see EraseQueuedLocked's identity check).
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
      // Exception firewall: a job that escapes with a C++ exception must never
      // propagate out of the worker functor (that is a std::terminate for the
      // whole app). Swallow it here and keep the worker draining. Jobs that need
      // to signal a waiter (RunOnWorkerBlocking) set their promise inside their
      // own body's try/catch so a throw cannot strand a UI-thread wait; this is
      // the last-resort net for everything else (plugin harvest bad_alloc, etc).
      try {
        job.task();
      } catch (const std::exception& ex) {
        std::fprintf(stderr, "[work-queue] job threw std::exception: %s\n", ex.what());
      } catch (...) {
        std::fprintf(stderr, "[work-queue] job threw a non-standard exception\n");
      }
    }
    if (hooks_.on_complete) {
      hooks_.on_complete();
    }
  }
}

}  // namespace microide::util
