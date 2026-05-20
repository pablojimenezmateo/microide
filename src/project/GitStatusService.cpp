#include "project/GitStatusService.h"

#include <filesystem>
#include <span>

#include "project/GitRepository.h"
#include "util/StartupTrace.h"

namespace microide::project {

std::unordered_map<std::string, GitFileStatus> CollectGitStatuses(
    const std::filesystem::path& root) {
  util::StartupTrace::Scope trace_scope("CollectGitStatuses");
  const GitRepository repo(root);
  if (!repo.IsValid()) {
    return {};
  }
  return repo.GetStatuses();
}

std::vector<GitWorkingTreeEntry> CollectGitWorkingTreeEntries(const std::filesystem::path& root) {
  const GitRepository repo(root);
  if (!repo.IsValid()) {
    return {};
  }
  return repo.GetWorkingTreeEntries();
}

std::unordered_map<std::string, GitFileStatus> BuildGitStatusMap(
    std::span<const GitWorkingTreeEntry> entries) {
  std::unordered_map<std::string, GitFileStatus> statuses;
  const auto priority = [](GitFileStatus status) {
    switch (status) {
      case GitFileStatus::Conflicted:
        return 5;
      case GitFileStatus::Deleted:
        return 4;
      case GitFileStatus::Modified:
        return 3;
      case GitFileStatus::Added:
      case GitFileStatus::Untracked:
        return 2;
      case GitFileStatus::Clean:
      default:
        return 0;
    }
  };
  const auto combine = [priority](GitFileStatus current, GitFileStatus next) {
    return priority(next) > priority(current) ? next : current;
  };
  const auto record = [&](std::filesystem::path relative_path, GitFileStatus status) {
    relative_path = relative_path.lexically_normal();
    const std::string normalized = relative_path.generic_string();
    if (!normalized.empty() && normalized != ".") {
      statuses[normalized] = combine(statuses[normalized], status);
    }

    std::filesystem::path dir = relative_path.parent_path();
    while (!dir.empty() && dir != ".") {
      const std::string key = dir.lexically_normal().generic_string();
      statuses[key] = combine(statuses[key], status);
      const auto next = dir.parent_path();
      if (next == dir) {
        break;
      }
      dir = next;
    }
  };

  for (const GitWorkingTreeEntry& entry : entries) {
    record(entry.relative_path, entry.conflicted ? GitFileStatus::Conflicted : entry.status);
  }
  return statuses;
}

bool GitStageAll(const std::filesystem::path& root) {
  const GitRepository repo(root);
  if (!repo.IsValid()) {
    return false;
  }
  return repo.StageAll();
}

bool GitStagePath(const std::filesystem::path& root, const std::filesystem::path& absolute_path) {
  const GitRepository repo(root);
  if (absolute_path.empty() || !repo.IsValid()) {
    return false;
  }

  const auto relative_path = repo.ToRelative(absolute_path);
  if (!relative_path.has_value()) {
    return false;
  }
  return repo.Stage(*relative_path);
}

bool GitUnstagePath(const std::filesystem::path& root, const std::filesystem::path& absolute_path) {
  const GitRepository repo(root);
  if (absolute_path.empty() || !repo.IsValid()) {
    return false;
  }

  const auto relative_path = repo.ToRelative(absolute_path);
  if (!relative_path.has_value()) {
    return false;
  }
  return repo.Unstage(*relative_path);
}

bool GitDiscardPath(const std::filesystem::path& root, const std::filesystem::path& absolute_path) {
  const GitRepository repo(root);
  if (absolute_path.empty() || !repo.IsValid()) {
    return false;
  }

  const auto relative_path = repo.ToRelative(absolute_path);
  if (!relative_path.has_value()) {
    return false;
  }
  return repo.Discard(*relative_path);
}

bool GitDiscardAll(const std::filesystem::path& root) {
  const GitRepository repo(root);
  if (!repo.IsValid()) {
    return false;
  }
  return repo.DiscardAll();
}

}  // namespace microide::project
