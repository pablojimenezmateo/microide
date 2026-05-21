#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>

namespace microide::tests::architecture {

RuleResult CheckWorkspaceShellCompanionTuCount(const std::filesystem::path& repo_root);
RuleResult CheckCoordinatorTuSize(const std::filesystem::path& repo_root);

}  // namespace microide::tests::architecture
