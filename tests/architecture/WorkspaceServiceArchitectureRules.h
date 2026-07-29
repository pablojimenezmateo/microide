#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>

namespace microide::tests::architecture {

RuleResult CheckRenderTuDoesNotMaterializeStrings(const std::filesystem::path& repo_root);
RuleResult CheckRenderTuDoesNotCallToStringOrFormat(const std::filesystem::path& repo_root);
RuleResult CheckTextViewportNoFullDocCopy(const std::filesystem::path& repo_root);
RuleResult CheckTextViewportApplyPipelineNoFullDocumentLineSnapshot(const std::filesystem::path& repo_root);
RuleResult CheckRenderTuEditorEssentialsAvoidEphemeralLabelStrings(const std::filesystem::path& repo_root);
RuleResult CheckWorkspaceShellRenderFrameAvoidsEphemeralEditorViewModelStrings(const std::filesystem::path& repo_root);
RuleResult CheckSdlTtfBackendNoPerGlyphLoop(const std::filesystem::path& repo_root);
RuleResult CheckDecoratedTextGridRendererBatchesFills(const std::filesystem::path& repo_root);
RuleResult CheckEditorViewRendererUsesScratchRows(const std::filesystem::path& repo_root);
RuleResult CheckCompareMergeRenderUsesScratchRows(const std::filesystem::path& repo_root);
RuleResult CheckApplicationCoalescesResize(const std::filesystem::path& repo_root);
RuleResult CheckMouseWheelUsesFractionalAccumulator(const std::filesystem::path& repo_root);
RuleResult CheckBottomPanelTerminalRectCache(const std::filesystem::path& repo_root);
RuleResult CheckNoStdStoInRenderOrBuilderTus(const std::filesystem::path& repo_root);
RuleResult CheckStatusBarRefreshIsAsyncOnly(const std::filesystem::path& repo_root);
RuleResult CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings(const std::filesystem::path& repo_root);
RuleResult CheckPaintedScrollbarsAreGrabbable(const std::filesystem::path& repo_root);
RuleResult CheckDebugSubsystemThreadingBehindDapClient(const std::filesystem::path& repo_root);

}  // namespace microide::tests::architecture
