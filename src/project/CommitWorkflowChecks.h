#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "project/CommitWorkflowTypes.h"
#include "project/GitRepositoryState.h"

namespace microide::project {

CommitStagedSummary BuildCommitStagedSummary(const GitRepositoryState& repository_state);

// `precomputed_summary`, when non-null, is used instead of re-running the staged-diff
// subprocess (git diff --cached --numstat) internally. Callers that already built the
// summary (e.g. the commit-workflow refresh) should pass it to avoid a redundant,
// byte-identical subprocess on the shell thread.
std::vector<CommitPreCheck> RunCommitPreChecks(
    const GitRepositoryState& repository_state,
    std::string_view subject,
    std::string_view body,
    const std::unordered_set<std::string>& acknowledged_warning_ids,
    const CommitStagedSummary* precomputed_summary = nullptr);

bool CommitPreChecksAllowExecution(const std::vector<CommitPreCheck>& checks,
                                   const std::unordered_set<std::string>& acknowledged_warning_ids);

bool StagedDiffContainsConflictMarkers(const std::filesystem::path& repository_root);

}  // namespace microide::project
