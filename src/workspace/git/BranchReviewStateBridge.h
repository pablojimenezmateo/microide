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

// What `state`'s project leaves on its host of its review marks and notes, for
// the user's other machine (workspace/persistence/RoamingReview.h): the targets
// of the repository at its root. Empty when it has none.
std::string RoamingReviewText(const ProjectWorkspaceState& state);
// Take what another machine left there, target by target, the copy changed last
// winning. True when `state`'s marks changed; its open compare tabs then show them.
bool TakeRoamingReview(std::string_view text, ProjectWorkspaceState* state);

std::optional<compare::BranchReviewTargetIdentity> OutgoingBranchReviewTarget(
    const GitSidebarState& git_state,
    const std::filesystem::path& repository_root);

}  // namespace microide::workspace
