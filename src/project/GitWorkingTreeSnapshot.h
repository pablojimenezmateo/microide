#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/ProcessLauncher.h"
#include "project/GitCompareService.h"

namespace microide::project {

inline constexpr std::string_view kGitReviewCheckpointRef = "refs/worktree/microide/review-checkpoint";

struct GitSnapshotResult {
  std::string commit;
  std::string error;

  bool ok() const { return !commit.empty(); }
};

GitSnapshotResult SnapshotGitWorkingTree(const std::filesystem::path& repository_root,
                                         const platform::ProcessLauncher& launcher);

GitSnapshotResult MarkGitReviewCheckpoint(const std::filesystem::path& repository_root,
                                          const platform::ProcessLauncher& launcher);

std::optional<std::string> ResolveGitReviewCheckpoint(const std::filesystem::path& repository_root,
                                                      const platform::ProcessLauncher& launcher);

std::vector<GitBranchFileEntry> CollectGitTreeDiffFiles(const std::filesystem::path& repository_root,
                                                        const platform::ProcessLauncher& launcher,
                                                        std::string_view from, std::string_view to);

}
