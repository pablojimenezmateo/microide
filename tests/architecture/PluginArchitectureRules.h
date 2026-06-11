#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>
#include <vector>

namespace microide::tests::architecture {

RuleResult CheckSinglePluginReloadPerActivation(const std::filesystem::path& repo_root);
RuleResult CheckEssentialEditorCppModulesDoNotTouchLuaState(const std::filesystem::path& repo_root);
RuleResult CheckPluginDrainBeforeTeardown(const std::filesystem::path& repo_root);
RuleResult CheckPluginTranslationUnitSize(const std::filesystem::path& repo_root);
RuleResult CheckNoProjectLocalPluginDiscovery(const std::filesystem::path& repo_root);
RuleResult CheckPluginLuaErrorDoesNotLongjmpOverCppLocals(const std::filesystem::path& repo_root);

std::vector<RuleResult> RunPluginArchitectureRules(const std::filesystem::path& repo_root);

}  // namespace microide::tests::architecture
