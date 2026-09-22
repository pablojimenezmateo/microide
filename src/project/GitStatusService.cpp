#include "project/GitStatusService.h"

#include <filesystem>
#include <span>

#include "project/GitPorcelainParser.h"
#include "project/GitRepository.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StartupTrace.h"

namespace microide::project {

std::vector<GitWorkingTreeEntry> CollectGitWorkingTreeEntries(const std::filesystem::path& root) {
  util::PerformanceTrace::Scope perf_scope("git::CollectGitWorkingTreeEntries");
  util::AddPerformanceCounter(util::PerfCounterId::GitStatusRefreshCalls);
  const GitRepository repo(root, platform::LocalProcessLauncher());
  if (!repo.IsValid()) {
    return {};
  }
  std::vector<GitWorkingTreeEntry> entries = repo.GetWorkingTreeEntries();
  util::AddPerformanceCounter(util::PerfCounterId::GitStatusEntriesParsed, entries.size());
  return entries;
}

bool GitStageAll(const std::filesystem::path& root) {
  const GitRepository repo(root, platform::LocalProcessLauncher());
  if (!repo.IsValid()) {
    return false;
  }
  return repo.StageAll();
}

bool GitStagePath(const std::filesystem::path& root, const std::filesystem::path& absolute_path) {
  const GitRepository repo(root, platform::LocalProcessLauncher());
  if (absolute_path.empty() || !repo.IsValid()) {
    return false;
  }

  const auto relative_path = repo.ToRelative(absolute_path);
  if (!relative_path.has_value()) {
    return false;
  }
  return repo.Stage(*relative_path);
}

bool GitUnstagePath(const std::filesystem::path& root, const std::filesystem::path& absolute_path,
                    bool may_be_staged_rename) {
  const GitRepository repo(root, platform::LocalProcessLauncher());
  if (absolute_path.empty() || !repo.IsValid()) {
    return false;
  }

  const auto relative_path = repo.ToRelative(absolute_path);
  if (!relative_path.has_value()) {
    return false;
  }
  return repo.Unstage(*relative_path, may_be_staged_rename);
}

bool GitDiscardPath(const std::filesystem::path& root, const std::filesystem::path& absolute_path,
                    bool may_be_staged_rename) {
  const GitRepository repo(root, platform::LocalProcessLauncher());
  if (absolute_path.empty() || !repo.IsValid()) {
    return false;
  }

  const auto relative_path = repo.ToRelative(absolute_path);
  if (!relative_path.has_value()) {
    return false;
  }
  return repo.Discard(*relative_path, may_be_staged_rename);
}

bool GitDiscardAll(const std::filesystem::path& root, bool remove_untracked) {
  const GitRepository repo(root, platform::LocalProcessLauncher());
  if (!repo.IsValid()) {
    return false;
  }
  return repo.DiscardAll(remove_untracked);
}

}  // namespace microide::project
