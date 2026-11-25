#pragma once

#include <span>
#include <vector>

#include "project/GitCompareService.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

std::vector<QuickPickItem> BuildCommitPickItems(std::span<const project::GitCommitEntry> commits);

}
