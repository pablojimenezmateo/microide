#pragma once

#include <filesystem>
#include <optional>

#include "compare/BranchReviewStateService.h"
#include "compare/CompareReviewTypes.h"
#include "workspace/BranchReviewPersistence.h"
#include "workspace/WorkspaceSidebarState.h"

namespace microide::workspace {

PersistedBranchReviewState ToPersistedBranchReviewState(
    const compare::BranchReviewStateService& service);
void LoadBranchReviewStateFromPersisted(const PersistedBranchReviewState& persisted,
                                        compare::BranchReviewStateService* service);

std::optional<compare::BranchReviewTargetIdentity> OutgoingBranchReviewTarget(
    const GitSidebarState& git_state,
    const std::filesystem::path& repository_root);

}  // namespace microide::workspace
