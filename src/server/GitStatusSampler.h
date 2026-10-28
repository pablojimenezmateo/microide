#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>

#include "project/GitRepositoryMetadataTracker.h"
#include "server/PeriodicSampler.h"

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
    sampler_.emplace("srv-git", interval, [this]() {
      if (!tracker_.SampleChanges().empty() && changed_) {
        changed_();
      }
    });
  }

 private:
  project::GitRepositoryMetadataTracker tracker_;  // the sampler thread's alone
  std::function<void()> changed_;
  std::optional<PeriodicSampler> sampler_;  // last: its thread uses the above
};

}  // namespace microide::server
