#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "project/DirectoryTree.h"

namespace microide::project {

struct GitWorkingTreeEntry {
  std::filesystem::path relative_path;
  GitFileStatus status = GitFileStatus::Clean;
  bool staged = false;
  bool conflicted = false;
};

// Blocking `git status` capture. The sidebar and the file tree do NOT come
// through here -- they read the porcelain v2 snapshot GitRepositoryService
// refreshes in the background. This is the one remaining synchronous caller
// (ReviewSessionCoordinator::OpenConflictReview, which needs the conflict set at
// the moment the action is invoked).
std::vector<GitWorkingTreeEntry> CollectGitWorkingTreeEntries(const std::filesystem::path& root);
bool GitStageAll(const std::filesystem::path& root);
bool GitStagePath(const std::filesystem::path& root, const std::filesystem::path& absolute_path);
bool GitUnstagePath(const std::filesystem::path& root, const std::filesystem::path& absolute_path,
                    bool may_be_staged_rename = true);
bool GitDiscardAll(const std::filesystem::path& root, bool remove_untracked = true);
bool GitDiscardPath(const std::filesystem::path& root, const std::filesystem::path& absolute_path,
                    bool may_be_staged_rename = true);

}  // namespace microide::project
