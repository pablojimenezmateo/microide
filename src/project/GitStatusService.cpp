#include "project/GitStatusService.h"

#include <filesystem>
#include <span>

#include "project/GitPorcelainParser.h"
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
  // Delegate file + folder aggregation to the canonical GitPorcelainParser helper so the
  // status-priority ranking stays single-sourced. A previous inline copy ranked
  // Added == Untracked, diverging from GitStatusPriority (Added > Untracked) and making the
  // folder-aggregated badge depend on which path produced it.
  std::unordered_map<std::string, GitFileStatus> statuses;
  for (const GitWorkingTreeEntry& entry : entries) {
    GitPorcelainParser::RecordGitStatus(
        statuses, entry.relative_path,
        entry.conflicted ? GitFileStatus::Conflicted : entry.status);
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
