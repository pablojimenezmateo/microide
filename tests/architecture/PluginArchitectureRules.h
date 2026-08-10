#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>
#include <vector>

namespace microide::tests::architecture {

RuleResult CheckSinglePluginReloadPerActivation(const std::filesystem::path& repo_root);
RuleResult CheckEssentialEditorCppModulesDoNotTouchLuaState(const std::filesystem::path& repo_root);
RuleResult CheckPluginTranslationUnitSize(const std::filesystem::path& repo_root);
RuleResult CheckNoProjectLocalPluginDiscovery(const std::filesystem::path& repo_root);
RuleResult CheckPluginLuaErrorDoesNotLongjmpOverCppLocals(const std::filesystem::path& repo_root);
RuleResult CheckPluginFieldReadsAreMetamethodProtected(const std::filesystem::path& repo_root);
RuleResult CheckLuaStaysBehindPluginBoundary(const std::filesystem::path& repo_root);
RuleResult CheckCoreIsNetworkFree(const std::filesystem::path& repo_root);

// Single source of truth for the plugin rules, so the test layer can register one
// ctest case per rule (sharding then runs the regex-heavy ones in parallel) while
// RunPluginArchitectureRules iterates the same list. As one aggregate case it took
// 221 s under TSAN with no contention, against the runner's 300 s per-test
// watchdog — a test that passes only on an idle machine (TD-2026-08-10-171).
const std::vector<NamedRule>& PluginArchitectureRuleList();

std::vector<RuleResult> RunPluginArchitectureRules(const std::filesystem::path& repo_root);

}  // namespace microide::tests::architecture
