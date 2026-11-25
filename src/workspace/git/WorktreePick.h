#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "project/GitWorktrees.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

std::vector<QuickPickItem> BuildWorktreePickItems(std::span<const project::GitWorktree> worktrees,
                                                  std::string_view current_root,
                                                  std::string_view open_command,
                                                  std::string_view target_prefix);

}
