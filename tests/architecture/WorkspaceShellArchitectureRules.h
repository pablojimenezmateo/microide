#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>

namespace microide::tests::architecture {

RuleResult CheckWorkspaceFriends(const std::filesystem::path& repo_root);
RuleResult CheckCoordinatorShellConstructors(const std::filesystem::path& repo_root);
RuleResult CheckThrowingStoParsers(const std::filesystem::path& repo_root);
RuleResult CheckEveryActionIdIsReachable(const std::filesystem::path& repo_root);
RuleResult CheckRenderSurfaceStateAccess(const std::filesystem::path& repo_root);
RuleResult CheckRenderSurfaceGeometryAccess(const std::filesystem::path& repo_root);
RuleResult CheckPerClipRenderPathDoesNotRunFramePrep(const std::filesystem::path& repo_root);
RuleResult CheckPersistenceFileIoBoundary(const std::filesystem::path& repo_root);

}  // namespace microide::tests::architecture
