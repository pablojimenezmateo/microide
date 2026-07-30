#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "project/ProjectChangeTypes.h"

namespace microide::project {

// The branch HEAD points at ("main"), read straight out of `<gitdir>/HEAD` — no
// subprocess. Follows a `.git` file to a linked worktree/submodule gitdir the same
// way the change tracker does. Returns nullopt for a detached HEAD (no branch to
// name) or a path that is not a repository. Used to label the status bar before the
// first `git status` snapshot exists, so a freshly-opened repo does not report
// itself as unversioned.
std::optional<std::string> ReadHeadBranchName(const std::filesystem::path& project_root);

class GitRepositoryMetadataTracker {
 public:
  void Reset();
  void SetProjectRoot(const std::filesystem::path& project_root);
  std::vector<RepositoryChange> SampleChanges();

 private:
  struct MetadataTick {
    std::uint64_t head = 0;
    std::uint64_t index = 0;
    // Tick of the branch ref HEAD points at (loose ref under the common gitdir). Catches
    // the common "same branch, new commit" case where `.git/HEAD` text is unchanged but
    // `refs/heads/<branch>` advances. (TD-2026-07-16-63.)
    std::uint64_t branch_ref = 0;
    // Tick of `packed-refs`: a fallback for branch refs stored packed rather than loose.
    std::uint64_t packed_refs = 0;
  };

  std::optional<MetadataTick> ReadCurrentTicks() const;

  std::filesystem::path project_root_;
  std::optional<MetadataTick> baseline_;
};

}  // namespace microide::project
