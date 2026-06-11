#include "architecture/WorkspaceArchitectureRules.h"

#include "architecture/WorkspaceCoordinatorArchitectureRules.h"
#include "architecture/WorkspaceFileSizeArchitectureRules.h"
#include "architecture/WorkspaceServiceArchitectureRules.h"
#include "architecture/WorkspaceShellArchitectureRules.h"
#include "architecture/WorkspaceViewModelArchitectureRules.h"

#include <vector>

namespace microide::tests::architecture {

std::vector<RuleResult> RunWorkspaceArchitectureRules(const std::filesystem::path& repo_root) {
  std::vector<RuleResult> results;
  const auto run = [&](auto&& fn) { results.push_back(fn(repo_root)); };
  run(CheckWorkspaceFriends);
  run(CheckCoordinatorShellConstructors);
  run(CheckThrowingStoParsers);
  run(CheckRenderSurfaceStateAccess);
  run(CheckRenderSurfaceGeometryAccess);
  run(CheckWorkspaceShellCompanionTuCount);
  run(CheckCoordinatorTuSize);
  run(CheckViewModelBackReferences);
  run(CheckCompareRenderStructuralGate);
  run(CheckPerClipRenderPathDoesNotRunFramePrep);
  run(CheckPersistenceFileIoBoundary);
  run(CheckNoSynchronousSubprocessWaitInWorkspace);
  run(CheckLspDidOpenIsNonBlocking);
  run(CheckTextViewportNoCombinedLayoutRevision);
  run(CheckNoLegacyPersistenceSymbols);
  run(CheckNoDebuggerDapSurface);
  run(CheckNoExecutorPostThenFutureGetInWorkspace);
  run(CheckNoSynchronousSubprocessInWorkspace);
  run(CheckNoDirectGitRepositoryInWorkspace);
  run(CheckOverlayDismissalIsCentralized);
  run(CheckRenderTuDoesNotMaterializeStrings);
  run(CheckRenderTuDoesNotCallToStringOrFormat);
  run(CheckTextViewportNoFullDocCopy);
  run(CheckTextViewportApplyPipelineNoFullDocumentLineSnapshot);
  run(CheckBuildEditorViewModelUsesIncrementalVectorWrites);
  run(CheckRenderTuEditorEssentialsAvoidEphemeralLabelStrings);
  run(CheckWorkspaceShellRenderFrameAvoidsEphemeralEditorViewModelStrings);
  run(CheckSdlTtfBackendNoPerGlyphLoop);
  run(CheckDecoratedTextGridRendererBatchesFills);
  run(CheckEditorViewRendererUsesScratchRows);
  run(CheckApplicationCoalescesResize);
  run(CheckMouseWheelUsesFractionalAccumulator);
  run(CheckBottomPanelTerminalRectCache);
  run(CheckNoStdStoInRenderOrBuilderTus);
  run(CheckStatusBarRefreshIsAsyncOnly);
  run(CheckSidebarSurfaceFallbackUsesStringView);
  run(CheckRenderTuDoesNotMaterializeSingleCharOrPrefixStrings);
  run(CheckEditorViewModelStickyAndOccurrenceAreSpans);
  return results;
}

}  // namespace microide::tests::architecture
