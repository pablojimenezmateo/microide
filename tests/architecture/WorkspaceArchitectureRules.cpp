#include "architecture/WorkspaceArchitectureRules.h"

#include "architecture/WorkspaceCoordinatorArchitectureRules.h"
#include "architecture/WorkspaceFileSizeArchitectureRules.h"
#include "architecture/WorkspaceServiceArchitectureRules.h"
#include "architecture/WorkspaceShellArchitectureRules.h"
#include "architecture/WorkspaceViewModelArchitectureRules.h"

#include <vector>

namespace microide::tests::architecture {

const std::vector<NamedRule>& WorkspaceArchitectureRuleList() {
  // Single source of truth for the workspace architecture rules. The test layer
  // registers one ctest case per entry (parallelized by sharding) and
  // RunWorkspaceArchitectureRules iterates the same list, so a new rule added
  // here is picked up by both without any second edit.
  static const std::vector<NamedRule> rules = {
      {"CheckWorkspaceFriends", CheckWorkspaceFriends},
      {"CheckCoordinatorShellConstructors", CheckCoordinatorShellConstructors},
      {"CheckThrowingStoParsers", CheckThrowingStoParsers},
      {"CheckDapTransportUsesCheckedResponseSeqNarrowing",
       CheckDapTransportUsesCheckedResponseSeqNarrowing},
      {"CheckPerfScenariosUseNonThrowingFilesystemProbes",
       CheckPerfScenariosUseNonThrowingFilesystemProbes},
      {"CheckOneShotWakeProducersCheckPushResultOrHaveBackstop",
       CheckOneShotWakeProducersCheckPushResultOrHaveBackstop},
      {"CheckPublicScriptsUseRunChecksForCtest", CheckPublicScriptsUseRunChecksForCtest},
      {"CheckRenderSurfaceStateAccess", CheckRenderSurfaceStateAccess},
      {"CheckRenderSurfaceGeometryAccess", CheckRenderSurfaceGeometryAccess},
      {"CheckWorkspaceShellCompanionTuCount", CheckWorkspaceShellCompanionTuCount},
      {"CheckCoordinatorTuSize", CheckCoordinatorTuSize},
      {"CheckDebugTuSize", CheckDebugTuSize},
      {"CheckViewModelBackReferences", CheckViewModelBackReferences},
      {"CheckCompareRenderStructuralGate", CheckCompareRenderStructuralGate},
      {"CheckPerClipRenderPathDoesNotRunFramePrep", CheckPerClipRenderPathDoesNotRunFramePrep},
      {"CheckPersistenceFileIoBoundary", CheckPersistenceFileIoBoundary},
      {"CheckNoSynchronousSubprocessWaitInWorkspace", CheckNoSynchronousSubprocessWaitInWorkspace},
      {"CheckLspDidOpenIsNonBlocking", CheckLspDidOpenIsNonBlocking},
      {"CheckTextViewportNoCombinedLayoutRevision", CheckTextViewportNoCombinedLayoutRevision},
      {"CheckNoLegacyPersistenceSymbols", CheckNoLegacyPersistenceSymbols},
      {"CheckNoExecutorPostThenFutureGetInWorkspace", CheckNoExecutorPostThenFutureGetInWorkspace},
      {"CheckNoSynchronousSubprocessInWorkspace", CheckNoSynchronousSubprocessInWorkspace},
      {"CheckNoDirectGitRepositoryInWorkspace", CheckNoDirectGitRepositoryInWorkspace},
      {"CheckOverlayDismissalIsCentralized", CheckOverlayDismissalIsCentralized},
      {"CheckReactivationDoesNotReloadPlugins", CheckReactivationDoesNotReloadPlugins},
      {"CheckNoFallbackEditorViewportSymbols", CheckNoFallbackEditorViewportSymbols},
      {"CheckRenderTuDoesNotMaterializeStrings", CheckRenderTuDoesNotMaterializeStrings},
      {"CheckRenderTuDoesNotCallToStringOrFormat", CheckRenderTuDoesNotCallToStringOrFormat},
      {"CheckTextViewportNoFullDocCopy", CheckTextViewportNoFullDocCopy},
      {"CheckTextViewportApplyPipelineNoFullDocumentLineSnapshot",
       CheckTextViewportApplyPipelineNoFullDocumentLineSnapshot},
      {"CheckBuildEditorViewModelUsesIncrementalVectorWrites",
       CheckBuildEditorViewModelUsesIncrementalVectorWrites},
      {"CheckRenderTuEditorEssentialsAvoidEphemeralLabelStrings",
       CheckRenderTuEditorEssentialsAvoidEphemeralLabelStrings},
      {"CheckWorkspaceShellRenderFrameAvoidsEphemeralEditorViewModelStrings",
       CheckWorkspaceShellRenderFrameAvoidsEphemeralEditorViewModelStrings},
      {"CheckSdlTtfBackendNoPerGlyphLoop", CheckSdlTtfBackendNoPerGlyphLoop},
      {"CheckDecoratedTextGridRendererBatchesFills", CheckDecoratedTextGridRendererBatchesFills},
      {"CheckEditorViewRendererUsesScratchRows", CheckEditorViewRendererUsesScratchRows},
      {"CheckCompareMergeRenderUsesScratchRows", CheckCompareMergeRenderUsesScratchRows},
      {"CheckApplicationCoalescesResize", CheckApplicationCoalescesResize},
      {"CheckMouseWheelUsesFractionalAccumulator", CheckMouseWheelUsesFractionalAccumulator},
      {"CheckBottomPanelTerminalRectCache", CheckBottomPanelTerminalRectCache},
      {"CheckNoStdStoInRenderOrBuilderTus", CheckNoStdStoInRenderOrBuilderTus},
      {"CheckStatusBarRefreshIsAsyncOnly", CheckStatusBarRefreshIsAsyncOnly},
      {"CheckSidebarSurfaceFallbackUsesStringView", CheckSidebarSurfaceFallbackUsesStringView},
      {"CheckRenderViewModelsOwnProjectState", CheckRenderViewModelsOwnProjectState},
      {"CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings",
       CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings},
      {"CheckEditorViewModelStickyAndOccurrenceAreSpans",
       CheckEditorViewModelStickyAndOccurrenceAreSpans},
      {"CheckDebugSubsystemThreadingBehindDapClient", CheckDebugSubsystemThreadingBehindDapClient},
  };
  return rules;
}

std::vector<RuleResult> RunWorkspaceArchitectureRules(const std::filesystem::path& repo_root) {
  const std::vector<NamedRule>& rules = WorkspaceArchitectureRuleList();
  std::vector<RuleResult> results;
  results.reserve(rules.size());
  for (const NamedRule& rule : rules) {
    results.push_back(rule.fn(repo_root));
  }
  return results;
}

}  // namespace microide::tests::architecture
