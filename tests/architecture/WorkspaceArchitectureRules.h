#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>
#include <vector>

namespace microide::tests::architecture {

RuleResult CheckWorkspaceFriends(const std::filesystem::path& repo_root);
RuleResult CheckCoordinatorShellConstructors(const std::filesystem::path& repo_root);
RuleResult CheckThrowingStoParsers(const std::filesystem::path& repo_root);
RuleResult CheckDapTransportUsesCheckedResponseSeqNarrowing(const std::filesystem::path& repo_root);
RuleResult CheckPerfScenariosUseNonThrowingFilesystemProbes(const std::filesystem::path& repo_root);
RuleResult CheckOneShotWakeProducersCheckPushResultOrHaveBackstop(
    const std::filesystem::path& repo_root);
RuleResult CheckPublicScriptsUseRunChecksForCtest(const std::filesystem::path& repo_root);
RuleResult CheckPerfMeasureBodiesDoNotBuildTheirOwnInput(const std::filesystem::path& repo_root);
RuleResult CheckPerfMeasureBodiesDoNotWaitOnWallClock(const std::filesystem::path& repo_root);
RuleResult CheckFactoryResultsAreNotCapturedByValue(const std::filesystem::path& repo_root);
RuleResult CheckPerfHarnessIsolatesBeforeConstructingTheShell(
    const std::filesystem::path& repo_root);
RuleResult CheckRenderSurfaceStateAccess(const std::filesystem::path& repo_root);
RuleResult CheckRenderSurfaceGeometryAccess(const std::filesystem::path& repo_root);
RuleResult CheckWorkspaceShellCompanionTuCount(const std::filesystem::path& repo_root);
RuleResult CheckCoordinatorTuSize(const std::filesystem::path& repo_root);
RuleResult CheckViewModelBackReferences(const std::filesystem::path& repo_root);
RuleResult CheckCompareRenderStructuralGate(const std::filesystem::path& repo_root);
RuleResult CheckPerClipRenderPathDoesNotRunFramePrep(const std::filesystem::path& repo_root);
RuleResult CheckPersistenceFileIoBoundary(const std::filesystem::path& repo_root);
RuleResult CheckNoSynchronousSubprocessWaitInWorkspace(const std::filesystem::path& repo_root);
RuleResult CheckLspDidOpenIsNonBlocking(const std::filesystem::path& repo_root);
RuleResult CheckTextViewportNoCombinedLayoutRevision(const std::filesystem::path& repo_root);
RuleResult CheckTextViewportSpecialMembersCoverEveryField(const std::filesystem::path& repo_root);
RuleResult CheckSerializeLinesDoesNotMaterializeSnapshot(const std::filesystem::path& repo_root);
RuleResult CheckNoLegacyPersistenceSymbols(const std::filesystem::path& repo_root);
RuleResult CheckNoExecutorPostThenFutureGetInWorkspace(const std::filesystem::path& repo_root);
RuleResult CheckNoSynchronousSubprocessInWorkspace(const std::filesystem::path& repo_root);
RuleResult CheckNoDirectGitRepositoryInWorkspace(const std::filesystem::path& repo_root);
RuleResult CheckOverlayDismissalIsCentralized(const std::filesystem::path& repo_root);
RuleResult CheckRenderTuDoesNotMaterializeStrings(const std::filesystem::path& repo_root);
RuleResult CheckRenderTuDoesNotCallToStringOrFormat(const std::filesystem::path& repo_root);
RuleResult CheckTextViewportNoFullDocCopy(const std::filesystem::path& repo_root);
RuleResult CheckTextViewportApplyPipelineNoFullDocumentLineSnapshot(const std::filesystem::path& repo_root);
RuleResult CheckBuildEditorViewModelUsesIncrementalVectorWrites(const std::filesystem::path& repo_root);
RuleResult CheckRenderTuEditorEssentialsAvoidEphemeralLabelStrings(const std::filesystem::path& repo_root);
RuleResult CheckWorkspaceShellRenderFrameAvoidsEphemeralEditorViewModelStrings(const std::filesystem::path& repo_root);
RuleResult CheckSdlTtfBackendNoPerGlyphLoop(const std::filesystem::path& repo_root);
RuleResult CheckDecoratedTextGridRendererBatchesFills(const std::filesystem::path& repo_root);
RuleResult CheckEditorViewRendererUsesScratchRows(const std::filesystem::path& repo_root);
RuleResult CheckApplicationCoalescesResize(const std::filesystem::path& repo_root);
RuleResult CheckMouseWheelUsesFractionalAccumulator(const std::filesystem::path& repo_root);
RuleResult CheckBottomPanelTerminalRectCache(const std::filesystem::path& repo_root);
RuleResult CheckNoStdStoInRenderOrBuilderTus(const std::filesystem::path& repo_root);
RuleResult CheckStatusBarRefreshIsAsyncOnly(const std::filesystem::path& repo_root);
RuleResult CheckSidebarSurfaceFallbackUsesStringView(const std::filesystem::path& repo_root);
RuleResult CheckHintSegmentsUseTheSharedSeparator(const std::filesystem::path& repo_root);
RuleResult CheckRenderViewModelsOwnProjectState(const std::filesystem::path& repo_root);
RuleResult CheckReactivationDoesNotReloadPlugins(const std::filesystem::path& repo_root);
RuleResult CheckNoFallbackEditorViewportSymbols(const std::filesystem::path& repo_root);
RuleResult CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings(const std::filesystem::path& repo_root);
RuleResult CheckEditorViewModelStickyAndOccurrenceAreSpans(const std::filesystem::path& repo_root);
RuleResult CheckEveryPerfCounterHasAProducer(const std::filesystem::path& repo_root);
RuleResult CheckViewportFiletypeGoesThroughTheViewportMemo(
    const std::filesystem::path& repo_root);

// The full ordered list of workspace rules, each with a stable name. Both
// RunWorkspaceArchitectureRules and the per-rule ctest registration iterate
// this, so adding a rule here wires it into both automatically.
const std::vector<NamedRule>& WorkspaceArchitectureRuleList();

std::vector<RuleResult> RunWorkspaceArchitectureRules(const std::filesystem::path& repo_root);

}  // namespace microide::tests::architecture
