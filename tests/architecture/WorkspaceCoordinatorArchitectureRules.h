#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>

namespace microide::tests::architecture {

RuleResult CheckNoSynchronousSubprocessWaitInWorkspace(const std::filesystem::path& repo_root);
RuleResult CheckLspDidOpenIsNonBlocking(const std::filesystem::path& repo_root);
RuleResult CheckTextViewportNoCombinedLayoutRevision(const std::filesystem::path& repo_root);
RuleResult CheckNoLegacyPersistenceSymbols(const std::filesystem::path& repo_root);
RuleResult CheckNoDebuggerDapSurface(const std::filesystem::path& repo_root);
RuleResult CheckNoExecutorPostThenFutureGetInWorkspace(const std::filesystem::path& repo_root);
RuleResult CheckNoSynchronousSubprocessInWorkspace(const std::filesystem::path& repo_root);

}  // namespace microide::tests::architecture
