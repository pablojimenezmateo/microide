#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

#include "project/GitRepositoryMetadataTracker.h"

namespace microide::server {

// Notices a repository moving under a served tree when nothing the server ran did
// it — an agent committing in a host terminal, a checkout over ssh — by sampling
// HEAD, the branch ref, the index and packed-refs (the editor's own
// GitRepositoryMetadataTracker) every `interval`. The worktree side is the
// workspace watch's; `.git` is outside it. `changed` runs on the sampler's thread.
class GitStatusSampler {
 public:
  GitStatusSampler(std::filesystem::path root, std::chrono::milliseconds interval,
                   std::function<void()> changed)
      : changed_(std::move(changed)) {
    tracker_.SetProjectRoot(root);
    (void)tracker_.SampleChanges();  // the baseline
    thread_ = std::thread([this, interval]() { Run(interval); });
  }
  ~GitStatusSampler() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  GitStatusSampler(const GitStatusSampler&) = delete;
  GitStatusSampler& operator=(const GitStatusSampler&) = delete;

 private:
  void Run(std::chrono::milliseconds interval) {
    std::unique_lock lock(mutex_);
    while (!cv_.wait_for(lock, interval, [this] { return stop_; })) {
      lock.unlock();
      if (!tracker_.SampleChanges().empty() && changed_) {
        changed_();
      }
      lock.lock();
    }
  }

  project::GitRepositoryMetadataTracker tracker_;  // the sampler thread's alone
  std::function<void()> changed_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::thread thread_;  // last
};

}  // namespace microide::server
