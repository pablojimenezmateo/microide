#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>
#include <vector>

namespace microide::tests::architecture {

RuleResult CheckKernelStaysFreeOfTheWindowingLibrary(const std::filesystem::path& repo_root);

RuleResult CheckEverySpawnGoesThroughAProcessLauncher(const std::filesystem::path& repo_root);
RuleResult CheckWorkersOutliveNothingTheyWrite(const std::filesystem::path& repo_root);

RuleResult CheckGitLayerDoesNotChooseLocality(const std::filesystem::path& repo_root);
RuleResult CheckGitMetadataIsAskedOfTheHost(const std::filesystem::path& repo_root);

RuleResult CheckEveryUserSaveRunsTheSamePreparation(const std::filesystem::path& repo_root);

RuleResult CheckProjectWritesGoThroughTheWriteGate(const std::filesystem::path& repo_root);

const std::vector<NamedRule>& KernelArchitectureRuleList();

}
