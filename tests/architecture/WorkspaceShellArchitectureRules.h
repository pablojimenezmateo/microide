#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>
#include <span>
#include <string_view>

namespace microide::tests::architecture {

RuleResult CheckWorkspaceFriends(const std::filesystem::path& repo_root);
RuleResult CheckCoordinatorShellConstructors(const std::filesystem::path& repo_root);
RuleResult CheckThrowingStoParsers(const std::filesystem::path& repo_root);
RuleResult CheckSdlDrawCallsStayInTheAllowlist(const std::filesystem::path& repo_root);
std::span<const std::string_view> SdlDrawAllowlist();
RuleResult CheckEveryActionIdIsReachable(const std::filesystem::path& repo_root);
RuleResult CheckRenderSurfaceStateAccess(const std::filesystem::path& repo_root);
RuleResult CheckRenderSurfaceGeometryAccess(const std::filesystem::path& repo_root);
RuleResult CheckPerClipRenderPathDoesNotRunFramePrep(const std::filesystem::path& repo_root);
RuleResult CheckPersistenceFileIoBoundary(const std::filesystem::path& repo_root);

}
