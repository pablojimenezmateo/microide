#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>
#include <vector>

namespace microide::tests::architecture {

RuleResult CheckTerminalSessionTuSize(const std::filesystem::path& repo_root);
RuleResult CheckTerminalSessionHeaderSize(const std::filesystem::path& repo_root);
RuleResult CheckTerminalSessionPrivateMethodCount(const std::filesystem::path& repo_root);
RuleResult CheckTerminalHelperTuSize(const std::filesystem::path& repo_root);
RuleResult CheckTerminalParserHelpersNoForbiddenDeps(const std::filesystem::path& repo_root);
RuleResult CheckTerminalSessionNoExtractedImpl(const std::filesystem::path& repo_root);
RuleResult CheckTerminalInternalHeadersStayInTerminalDir(const std::filesystem::path& repo_root);
RuleResult CheckTerminalSessionSplitTranslationUnits(const std::filesystem::path& repo_root);
RuleResult CheckArchitectureInvariantsDispatcherSize(const std::filesystem::path& repo_root);
RuleResult CheckArchitectureRulesTuSize(const std::filesystem::path& repo_root);
RuleResult CheckWorkspaceArchitectureRulesDispatcherSize(const std::filesystem::path& repo_root);
RuleResult CheckDescriptorCreationIsCloseOnExec(const std::filesystem::path& repo_root);

std::vector<RuleResult> RunTerminalArchitectureRules(const std::filesystem::path& repo_root);

}  // namespace microide::tests::architecture
