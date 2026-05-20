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

std::unordered_map<std::string, GitFileStatus> CollectGitStatuses(
    const std::filesystem::path& root);
std::vector<GitWorkingTreeEntry> CollectGitWorkingTreeEntries(const std::filesystem::path& root);
std::unordered_map<std::string, GitFileStatus> BuildGitStatusMap(
    std::span<const GitWorkingTreeEntry> entries);
bool GitStageAll(const std::filesystem::path& root);
bool GitStagePath(const std::filesystem::path& root, const std::filesystem::path& absolute_path);
bool GitUnstagePath(const std::filesystem::path& root, const std::filesystem::path& absolute_path);
bool GitDiscardAll(const std::filesystem::path& root);
bool GitDiscardPath(const std::filesystem::path& root, const std::filesystem::path& absolute_path);

}  // namespace microide::project
