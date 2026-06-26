#include "plugin/PluginThread.h"

#include <algorithm>
#include <utility>

namespace microide::plugin {

PluginThread::~PluginThread() { Shutdown(); }

void PluginThread::SetWakeEventType(Uint32 event_type) {
  wake_event_type_.store(event_type, std::memory_order_release);
}

void PluginThread::EnsureStarted() {
  std::lock_guard lock(inbound_mutex_);
  if (started_.load(std::memory_order_acquire) || stop_) {
    return;
  }
  worker_ = std::thread([this]() { WorkerMain(); });
  started_.store(true, std::memory_order_release);
}

void PluginThread::Post(std::function<void()> task) {
  {
    std::lock_guard lock(inbound_mutex_);
    if (stop_) {
      return;
    }
    inbound_.push_back(Job{.key = {}, .task = std::move(task), .cancelled = false});
  }
  inbound_cv_.notify_one();
}

void PluginThread::PostLatest(std::string key, std::function<void()> task) {
  {
    std::lock_guard lock(inbound_mutex_);
    if (stop_) {
      return;
    }
    inbound_.erase(std::remove_if(inbound_.begin(), inbound_.end(),
                                  [&key](const Job& job) {
                                    return !job.cancelled && job.key == key;
                                  }),
                   inbound_.end());
    inbound_.push_back(Job{.key = std::move(key), .task = std::move(task), .cancelled = false});
  }
  inbound_cv_.notify_one();
}

void PluginThread::PostToMain(PluginMainThreadAction action) {
  {
    std::lock_guard lock(mailbox_mutex_);
    mailbox_.push_back(std::move(action));
    mailbox_queued_.store(static_cast<int>(mailbox_.size()), std::memory_order_release);
  }
  const Uint32 wake = wake_event_type_.load(std::memory_order_acquire);
  if (wake != 0) {
    SDL_Event event{};
    event.type = wake;
    SDL_PushEvent(&event);
  }
}

int PluginThread::DrainMainThreadActions() {
  std::vector<PluginMainThreadAction> actions;
  {
    std::lock_guard lock(mailbox_mutex_);
    actions.swap(mailbox_);
    mailbox_queued_.store(0, std::memory_order_release);
  }
  for (auto& action : actions) {
    if (action) {
      action();
    }
  }
  return static_cast<int>(actions.size());
}

void PluginThread::Shutdown(std::chrono::milliseconds deadline) {
  {
    std::lock_guard lock(inbound_mutex_);
    if (stop_) {
      return;
    }
    for (auto& job : inbound_) {
      job.cancelled = true;
    }
    stop_ = true;
  }
  inbound_cv_.notify_all();
  if (worker_.joinable()) {
    // std::thread has no timed join; the per-call watchdog keeps in-flight jobs
    // bounded so this blocking join stays prompt for well-behaved plugins.
    (void)deadline;
    worker_.join();
  }
}

void PluginThread::WorkerMain() {
  while (true) {
    Job job;
    {
      std::unique_lock lock(inbound_mutex_);
      inbound_cv_.wait(lock, [this] { return stop_ || !inbound_.empty(); });
      if (stop_ && inbound_.empty()) {
        return;
      }
      if (inbound_.empty()) {
        continue;
      }
      job = std::move(inbound_.front());
      inbound_.pop_front();
    }
    if (!job.cancelled && job.task) {
      job.task();
    }
  }
}

}  // namespace microide::plugin
