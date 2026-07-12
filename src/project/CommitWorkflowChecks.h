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
//
// `scan_staged_diff_for_conflict_markers` gates the ConflictMarkers pre-check, which runs
// a full `git diff --cached` (unbounded output) on the calling thread. That is far more
// expensive than the bounded `--numstat` summary, so interactive refreshes (every
// keystroke in the commit subject/body) pass false and only the pre-dispatch check pays
// for it. The check is blocking, so surfacing it at commit time still prevents the commit.
std::vector<CommitPreCheck> RunCommitPreChecks(
    const GitRepositoryState& repository_state,
    std::string_view subject,
    std::string_view body,
    const std::unordered_set<std::string>& acknowledged_warning_ids,
    const CommitStagedSummary* precomputed_summary = nullptr,
    bool scan_staged_diff_for_conflict_markers = true);

bool CommitPreChecksAllowExecution(const std::vector<CommitPreCheck>& checks,
                                   const std::unordered_set<std::string>& acknowledged_warning_ids);

bool StagedDiffContainsConflictMarkers(const std::filesystem::path& repository_root);

}  // namespace microide::project
