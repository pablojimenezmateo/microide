#include "server/WorkspaceWatch.h"

#include <utility>

#include "project/ProjectTraversalFilter.h"

namespace microide::server {

WorkspaceWatch::WorkspaceWatch(std::filesystem::path root, Options options,
                               std::function<void()> changed)
    : options_(options), changed_(std::move(changed)), watcher_(options.poll_interval) {
  auto filter = std::make_shared<project::ProjectTraversalFilter>(root);
  // The watcher walks on its own threads; the filter's directory cache is not
  // thread-safe, so each call takes the lock.
  auto filter_mutex = std::make_shared<std::mutex>();
  watcher_.SetEntryFilter([filter, filter_mutex](const std::filesystem::path& path,
                                                 platform::PathType type) {
    std::lock_guard lock(*filter_mutex);
    return filter->Includes(path, type);
  });
  watcher_.SetWakeCallback([this]() {
    {
      std::lock_guard lock(mutex_);
      woken_ = true;
    }
    cv_.notify_all();
  });
  watcher_.SetRoots({std::move(root)});
  thread_ = std::thread([this]() { Run(); });
}

WorkspaceWatch::~WorkspaceWatch() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  watcher_.SetWakeCallback({});
  watcher_.Clear();
}

bool WorkspaceWatch::native() const {
  return !watcher_.NextPollDelay().has_value() && !watcher_.TreeTooLarge();
}

void WorkspaceWatch::Run() {
  std::unique_lock lock(mutex_);
  while (!stop_) {
    const std::optional<std::chrono::milliseconds> poll = watcher_.NextPollDelay();
    const auto wait = poll.has_value() ? std::max(*poll, std::chrono::milliseconds(50))
                                       : std::chrono::milliseconds(60'000);
    cv_.wait_for(lock, wait, [this] { return stop_ || woken_; });
    if (stop_) {
      return;
    }
    const bool woken = std::exchange(woken_, false);
    if (woken) {
      // Let the burst settle: every further event restarts the quiet period, up to
      // max_delay after the first.
      const auto first = std::chrono::steady_clock::now();
      while (!stop_) {
        const auto deadline =
            std::min(std::chrono::steady_clock::now() + options_.quiet, first + options_.max_delay);
        if (!cv_.wait_until(lock, deadline, [this] { return stop_ || woken_; })) {
          break;  // quiet
        }
        woken_ = false;
        if (std::chrono::steady_clock::now() >= first + options_.max_delay) {
          break;
        }
      }
      if (stop_) {
        return;
      }
    }
    lock.unlock();
    // Poll re-arms native watches for directories created since, and is the whole
    // mechanism when polling.
    const bool polled_change = watcher_.Poll();
    if ((woken || polled_change) && changed_) {
      changed_();
    }
    lock.lock();
  }
}

}  // namespace microide::server
