#pragma once

#include <span>
#include <vector>

#include "project/GitCompareService.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

// The rows of Git: Review Commit…: one per recent commit, newest first, each
// running `review-commit <hash>` -- a compare tab per file the commit changed. A
// row is the commit's subject, then its short hash, its age and its author:
// what an agent committed while nobody watched, one Enter from being read.
std::vector<QuickPickItem> BuildCommitPickItems(std::span<const project::GitCommitEntry> commits);

}  // namespace microide::workspace
