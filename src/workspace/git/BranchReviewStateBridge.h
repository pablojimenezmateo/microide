#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "compare/BranchReviewStateService.h"
#include "compare/CompareReviewTypes.h"
#include "workspace/git/BranchReviewPersistence.h"
#include "workspace/state/WorkspaceSidebarState.h"

namespace microide::workspace {

PersistedBranchReviewState ToPersistedBranchReviewState(
    const compare::BranchReviewStateService& service);
void LoadBranchReviewStateFromPersisted(const PersistedBranchReviewState& persisted,
                                        compare::BranchReviewStateService* service);

struct ProjectWorkspaceState;

std::string RoamingReviewText(const ProjectWorkspaceState& state);
bool TakeRoamingReview(std::string_view text, ProjectWorkspaceState* state);

std::optional<compare::BranchReviewTargetIdentity> OutgoingBranchReviewTarget(
    const GitSidebarState& git_state,
    const std::filesystem::path& repository_root);

}
