#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>
#include <vector>

namespace microide::tests::architecture {

// The kernel — the SDL-free half of the tree — must not reach the windowing
// library, directly or through any header it includes. See the rule body for what
// "kernel" means and why it is a header-graph check rather than a directory rule.
RuleResult CheckKernelStaysFreeOfTheWindowingLibrary(const std::filesystem::path& repo_root);

// Every spawn goes through a platform::ProcessLauncher, so a project can decide where
// its processes run. See the rule body for the two sanctioned exceptions.
RuleResult CheckEverySpawnGoesThroughAProcessLauncher(const std::filesystem::path& repo_root);

// A user-initiated save runs the same save participants and format-on-save wherever
// it was triggered from, and refuses the same disk conflicts. See the rule body.
RuleResult CheckEveryUserSaveRunsTheSamePreparation(const std::filesystem::path& repo_root);

const std::vector<NamedRule>& KernelArchitectureRuleList();

}  // namespace microide::tests::architecture
