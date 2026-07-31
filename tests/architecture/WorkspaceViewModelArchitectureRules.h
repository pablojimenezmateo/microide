#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>

namespace microide::tests::architecture {

RuleResult CheckViewModelBackReferences(const std::filesystem::path& repo_root);
RuleResult CheckCompareRenderStructuralGate(const std::filesystem::path& repo_root);
RuleResult CheckBuildEditorViewModelUsesIncrementalVectorWrites(const std::filesystem::path& repo_root);
RuleResult CheckSidebarSurfaceFallbackUsesStringView(const std::filesystem::path& repo_root);
RuleResult CheckMenuItemTextResolutionIsAllocationFree(const std::filesystem::path& repo_root);
RuleResult CheckRenderViewModelsOwnProjectState(const std::filesystem::path& repo_root);
RuleResult CheckEditorViewModelStickyAndOccurrenceAreSpans(const std::filesystem::path& repo_root);
RuleResult CheckHintSegmentsUseTheSharedSeparator(const std::filesystem::path& repo_root);

}  // namespace microide::tests::architecture
