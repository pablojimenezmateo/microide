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

// Blocking `git status` capture. The sidebar and the file tree do NOT come
// through here -- they read the porcelain v2 snapshot GitRepositoryService
// refreshes in the background. This is the one remaining synchronous caller
// (ReviewSessionCoordinator::OpenConflictReview, which needs the conflict set at
// the moment the action is invoked).
// `nullopt` when git could not be asked — see GitRepository::GetWorkingTreeEntries.
// A caller that treats it as an empty tree reports "no changes" for a question it
// never got an answer to.
//
// Every entry point takes the launcher explicitly — there is NO default. A
// default is exactly what G2 removed from `GitRepository`, because a spawn that
// can quietly inherit locality is a spawn that runs on the wrong machine the day
// a project is remote. Callers pass `ProjectWorkspaceState::launcher()`; a caller
// that must stay local whatever the project says `platform::LocalProcessLauncher()`
// and says why. (TD-2026-09-22-301.)
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

}  // namespace microide::project
