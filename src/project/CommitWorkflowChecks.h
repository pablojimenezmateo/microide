#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "platform/ProcessLauncher.h"
#include "project/CommitWorkflowTypes.h"
#include "project/GitRepositoryState.h"

namespace microide::project {

CommitStagedSummary BuildCommitStagedSummary(const GitRepositoryState& repository_state,
                                             const platform::ProcessLauncher& launcher);

std::vector<CommitPreCheck> RunCommitPreChecks(
    const GitRepositoryState& repository_state,
    const platform::ProcessLauncher& launcher,
    std::string_view subject,
    std::string_view body,
    const std::unordered_set<std::string>& acknowledged_warning_ids,
    const CommitStagedSummary* precomputed_summary = nullptr,
    bool scan_staged_diff_for_conflict_markers = true);

bool CommitPreChecksAllowExecution(const std::vector<CommitPreCheck>& checks,
                                   const std::unordered_set<std::string>& acknowledged_warning_ids);

std::optional<bool> StagedDiffContainsConflictMarkers(
    const std::filesystem::path& repository_root, const platform::ProcessLauncher& launcher);

}
