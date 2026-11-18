#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/ProcessLauncher.h"
#include "project/GitCompareService.h"

namespace microide::project {

// Where a working tree's review checkpoint is kept. `refs/worktree/` is git's
// per-working-tree namespace: each linked tree -- each agent's -- has its own.
inline constexpr std::string_view kGitReviewCheckpointRef = "refs/worktree/microide/review-checkpoint";

struct GitSnapshotResult {
  // The snapshot's commit id; empty on failure.
  std::string commit;
  // One sentence, when it failed.
  std::string error;

  bool ok() const { return !commit.empty(); }
};

// The working tree as it is now -- every tracked file's content and every
// untracked file `.gitignore` does not exclude -- as a commit no branch holds.
// The tree, the index and HEAD are not touched: the files are hashed through an
// index of the snapshot's own (`<gitdir>/microide-snapshot-index`, seeded once
// from the real one so only files that changed are read again).
//
// This is what "since I last looked" needs and neither HEAD nor a branch gives:
// agents commit, amend and rebase, and half of what they leave is not committed
// or not yet tracked. Two snapshots compare as two trees, whatever happened to
// the history between them.
GitSnapshotResult SnapshotGitWorkingTree(const std::filesystem::path& repository_root,
                                         const platform::ProcessLauncher& launcher);

// Snapshots the working tree and records it as the review checkpoint.
GitSnapshotResult MarkGitReviewCheckpoint(const std::filesystem::path& repository_root,
                                          const platform::ProcessLauncher& launcher);

// The checkpoint's commit id; nullopt when none was marked in this working tree.
std::optional<std::string> ResolveGitReviewCheckpoint(const std::filesystem::path& repository_root,
                                                      const platform::ProcessLauncher& launcher);

// The files that differ between two commits' trees (renames followed).
std::vector<GitBranchFileEntry> CollectGitTreeDiffFiles(const std::filesystem::path& repository_root,
                                                        const platform::ProcessLauncher& launcher,
                                                        std::string_view from, std::string_view to);

}  // namespace microide::project
