#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "project/GitWorktrees.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

// The rows of Git: Switch Worktree…: one per working tree that can be opened (a
// bare entry has no tree, a prunable one no directory), each running
// `<open_command> <target_prefix><path>` -- `project-open /repo-wt` for a local
// project, `remote-open host:/repo-wt` for one on a host. A row names the
// directory, then the branch (or the detached commit), where it is, and whether
// it is the one this project is.
std::vector<QuickPickItem> BuildWorktreePickItems(std::span<const project::GitWorktree> worktrees,
                                                  std::string_view current_root,
                                                  std::string_view open_command,
                                                  std::string_view target_prefix);

}  // namespace microide::workspace
