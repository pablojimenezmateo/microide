#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "platform/ProcessLauncher.h"
#include "project/DirectoryTree.h"

namespace microide::project {

struct GitWorkingTreeEntry {
  std::filesystem::path relative_path;
  GitFileStatus status = GitFileStatus::Clean;
  bool staged = false;
  bool conflicted = false;
};

std::optional<std::vector<GitWorkingTreeEntry>> CollectGitWorkingTreeEntries(
    const std::filesystem::path& root, const platform::ProcessLauncher& launcher);
bool GitStageAll(const std::filesystem::path& root, const platform::ProcessLauncher& launcher);
bool GitStagePath(const std::filesystem::path& root, const platform::ProcessLauncher& launcher,
                  const std::filesystem::path& absolute_path);
bool GitUnstagePath(const std::filesystem::path& root, const platform::ProcessLauncher& launcher,
                    const std::filesystem::path& absolute_path,
                    bool may_be_staged_rename = true);
bool GitDiscardAll(const std::filesystem::path& root, const platform::ProcessLauncher& launcher,
                   bool remove_untracked = true);
bool GitDiscardPath(const std::filesystem::path& root, const platform::ProcessLauncher& launcher,
                    const std::filesystem::path& absolute_path,
                    bool may_be_staged_rename = true);

}
