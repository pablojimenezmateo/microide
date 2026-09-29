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
  // What the tracker compares between samples to decide whether the repository
  // moved. HEAD and the branch ref are compared by CONTENT, the other two by
  // modification tick — see ReadCurrentFingerprint for why the split is where it is.
  struct MetadataFingerprint {
    // First line of `.git/HEAD`: either `ref: refs/heads/<branch>` or a detached
    // object id. Content, not a tick: `git checkout` of the branch already checked
    // out rewrites this file with the same bytes, and a tick comparison then
    // reported a HEAD change and spawned a `git status` that had nothing to find.
    std::string head_text;
    // First line of the loose ref HEAD points at (an object id), under the COMMON
    // gitdir for linked worktrees. Content for the same reason, and it is what
    // catches the "same branch, new commit" case where `.git/HEAD` never changes.
    // (TD-2026-07-16-63.)
    std::string branch_ref_text;
    // These two stay ticks. The index is routinely megabytes and `packed-refs` can
    // be large, so reading them to compare would cost more than the redundant
    // refresh it avoids — and neither is rewritten with identical content the way
    // HEAD is. (TD-2026-09-29-306.)
    std::uint64_t index = 0;
    std::uint64_t packed_refs = 0;
  };

  std::optional<MetadataFingerprint> ReadCurrentFingerprint() const;

  std::filesystem::path project_root_;
  std::optional<MetadataFingerprint> baseline_;
};

}  // namespace microide::project
