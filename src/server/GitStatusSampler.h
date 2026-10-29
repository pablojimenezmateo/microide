#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

#include "project/GitRepositoryMetadataTracker.h"
#include "util/WakePipe.h"

namespace microide::server {

// Notices a repository moving under a served tree when nothing the server ran did
// it — an agent committing in a host terminal, a checkout over ssh — by comparing
// HEAD, the branch ref, the index and packed-refs (the editor's own
// GitRepositoryMetadataTracker). The worktree side is the workspace watch's; `.git`
// is outside it. `changed` runs on the sampler's thread.
//
// Event-driven: the thread sleeps on inotify watches of the few directories those
// files live in (GitRepositoryMetadataTracker::WatchedDirectories) and samples when
// one of them changes. It stat-polled every second, which made it an idle server's
// largest wakeup source (TD-2026-10-28-418 item 4). `safety_interval` still samples
// now and then, for what no watch can see (a ref directory that did not exist yet,
// an inotify queue overflow, a filesystem without inotify), and is the whole
// mechanism where inotify cannot be had.
class GitStatusSampler {
 public:
  GitStatusSampler(std::filesystem::path root, std::chrono::milliseconds safety_interval,
                   std::function<void()> changed);
  ~GitStatusSampler();
  GitStatusSampler(const GitStatusSampler&) = delete;
  GitStatusSampler& operator=(const GitStatusSampler&) = delete;

 private:
  void Run(std::chrono::milliseconds safety_interval);
  void Sample();

  project::GitRepositoryMetadataTracker tracker_;  // the sampler thread's alone
  std::function<void()> changed_;
  std::atomic<bool> stopping_{false};
  util::WakePipe stop_;
  std::thread thread_;  // last: its thread uses the above
};

}  // namespace microide::server
