#include "TestRunner.h"
#include "TestSupport.h"
#include "TestSupportSdl.h"
#include "app/SdlWaker.h"

#include <SDL3/SDL.h>

#include <exception>
#include <optional>
#include <string>
#include <vector>

// The shell's test binary: everything that needs SDL, the shell, the editor's
// view layer, plugins or rendering. Tests of the SDL-free kernel live in
// microide_kernel_tests (tests/KernelTestMain.cpp); both share tests/TestRunner.cpp,
// so they take the same command line and shard the same way.

namespace microide::tests {

void RegisterApplicationTests(std::vector<TestCase>& tests);
void RegisterAppStartupOptionsTests(std::vector<TestCase>& tests);
void RegisterCompareReviewTests(std::vector<TestCase>& tests);
void RegisterBranchReviewStateTests(std::vector<TestCase>& tests);
void RegisterAssistServiceTests(std::vector<TestCase>& tests);
void RegisterParityTests(std::vector<TestCase>& tests);
void RegisterPatchApplyTests(std::vector<TestCase>& tests);
void RegisterDiagnosticsStoreTests(std::vector<TestCase>& tests);
void RegisterPluginDecorationStoreTests(std::vector<TestCase>& tests);
void RegisterPluginSurfaceStoreTests(std::vector<TestCase>& tests);
void RegisterPluginSurfacePreviewTests(std::vector<TestCase>& tests);
void RegisterPluginDisplayListTests(std::vector<TestCase>& tests);
void RegisterEditorRowYLayoutTests(std::vector<TestCase>& tests);
void RegisterEditorInsetLayoutTests(std::vector<TestCase>& tests);
void RegisterEolDecorationLayoutTests(std::vector<TestCase>& tests);
void RegisterInlayHintColumnsTests(std::vector<TestCase>& tests);
void RegisterBreakpointStoreTests(std::vector<TestCase>& tests);
void RegisterDirtyRegionPolicyTests(std::vector<TestCase>& tests);
void RegisterEditorSplitTreeTests(std::vector<TestCase>& tests);
void RegisterTerminalPaneLayoutTests(std::vector<TestCase>& tests);
void RegisterRecentsServiceTests(std::vector<TestCase>& tests);
void RegisterGitServiceTests(std::vector<TestCase>& tests);
void RegisterRuntimeSyntaxSkipTests(std::vector<TestCase>& tests);
void RegisterLineIndentScanTests(std::vector<TestCase>& tests);
void RegisterPersistedRecordWriteQueueTests(std::vector<TestCase>& tests);
void RegisterSyntaxDefinitionLoaderTests(std::vector<TestCase>& tests);
void RegisterGitRepositoryServiceTests(std::vector<TestCase>& tests);
void RegisterCommitWorkflowTests(std::vector<TestCase>& tests);
void RegisterGitSidebarCommandCenterTests(std::vector<TestCase>& tests);
void RegisterWorkspaceLspClientTests(std::vector<TestCase>& tests);
void RegisterLspFileWatchTests(std::vector<TestCase>& tests);
void RegisterLspProtocolTests(std::vector<TestCase>& tests);
void RegisterLspResourceOpsTests(std::vector<TestCase>& tests);
void RegisterLspPositionEncodingTests(std::vector<TestCase>& tests);
void RegisterLspRealServerE2ETests(std::vector<TestCase>& tests);
void RegisterDapRealAdapterE2ETests(std::vector<TestCase>& tests);
void RegisterWorkspaceDapClientTests(std::vector<TestCase>& tests);
void RegisterDapProtocolTests(std::vector<TestCase>& tests);
void RegisterDebugServiceTests(std::vector<TestCase>& tests);
void RegisterMergeModelTests(std::vector<TestCase>& tests);
void RegisterSurfaceTokenWindowTests(std::vector<TestCase>& tests);
void RegisterMergeConflictResolutionTests(std::vector<TestCase>& tests);
void RegisterReviewTabPlanTests(std::vector<TestCase>& tests);
void RegisterReviewSessionTests(std::vector<TestCase>& tests);
void RegisterPluginHostTests(std::vector<TestCase>& tests);
void RegisterPluginPureHelperTests(std::vector<TestCase>& tests);
void RegisterPluginSurfaceCoverageTests(std::vector<TestCase>& tests);
void RegisterSurfaceTextureCacheTests(std::vector<TestCase>& tests);
void RegisterExternalRepoChangeTests(std::vector<TestCase>& tests);
void RegisterMainThreadMailboxTests(std::vector<TestCase>& tests);
void RegisterRenderViewModelBuilderTests(std::vector<TestCase>& tests);
void RegisterRowDecorationBuilderTests(std::vector<TestCase>& tests);
void RegisterTerminalWordSelectionTests(std::vector<TestCase>& tests);
void RegisterThemeTests(std::vector<TestCase>& tests);
void RegisterTextRendererTests(std::vector<TestCase>& tests);
void RegisterTextViewportTests(std::vector<TestCase>& tests);
void RegisterTextLayoutTests(std::vector<TestCase>& tests);
void RegisterPieceTreeTests(std::vector<TestCase>& tests);
void RegisterMenuAcceleratorConsistencyTests(std::vector<TestCase>& tests);
void RegisterCommandLabelConsistencyTests(std::vector<TestCase>& tests);
void RegisterWorkspaceMenuRegistryTests(std::vector<TestCase>& tests);
void RegisterWorkspaceSettingsRegistryTests(std::vector<TestCase>& tests);
void RegisterSettingsStoreTests(std::vector<TestCase>& tests);
void RegisterWorkspaceTestControllerTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellChromeTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellEditorBlameTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellPluginTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellPromptTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellCompareTests(std::vector<TestCase>& tests);
void RegisterWorkspaceFileDropTests(std::vector<TestCase>& tests);
void RegisterColumnSelectionTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellRenderSurfaceTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellCursorTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellProjectTests(std::vector<TestCase>& tests);
void RegisterEditorGroupStateTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSearchTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSessionTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedCoreTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedLayoutTests(std::vector<TestCase>& tests);
void RegisterDebugPaneTests(std::vector<TestCase>& tests);
void RegisterDebugPaneRenderTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellRenderMenusTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellRenderPromptsTests(std::vector<TestCase>& tests);
void RegisterEditorHoverTargetTests(std::vector<TestCase>& tests);
void RegisterEditorBracketJumpTests(std::vector<TestCase>& tests);
void RegisterEditorDiagnosticNavigationTests(std::vector<TestCase>& tests);
void RegisterEditorUndoRedoWiringTests(std::vector<TestCase>& tests);
void RegisterEditorCopySurfacePriorityTests(std::vector<TestCase>& tests);
void RegisterEditorPasteBehaviorTests(std::vector<TestCase>& tests);
void RegisterEditorCompareSourceTests(std::vector<TestCase>& tests);
void RegisterEditorGoToLineTests(std::vector<TestCase>& tests);
void RegisterTabStripServiceTests(std::vector<TestCase>& tests);
void RegisterTextDragDropTests(std::vector<TestCase>& tests);
void RegisterSelectionGranularityTests(std::vector<TestCase>& tests);
void RegisterSelectionGranularityPropertyTests(std::vector<TestCase>& tests);
void RegisterMergeWrapRowsTests(std::vector<TestCase>& tests);
void RegisterAsyncFileOpenTests(std::vector<TestCase>& tests);
void RegisterDiffWrapLayoutTests(std::vector<TestCase>& tests);
void RegisterImeCompositionTests(std::vector<TestCase>& tests);
void RegisterSearchDifferentialTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedSearchTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSourceControlTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedTerminalTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellTerminalTests(std::vector<TestCase>& tests);
void RegisterWorkspaceStatusBarTests(std::vector<TestCase>& tests);
void RegisterNotificationServiceTests(std::vector<TestCase>& tests);
void RegisterContributionRegistryTests(std::vector<TestCase>& tests);
void RegisterPhase4Tests(std::vector<TestCase>& tests);
void RegisterPhase5Tests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellFormatJsonTests(std::vector<TestCase>& tests);
void RegisterSaveDataIntegrityTests(std::vector<TestCase>& tests);
void RegisterControlClientTests(std::vector<TestCase>& tests);
void RegisterControlProtocolRobustnessTests(std::vector<TestCase>& tests);
void RegisterControlProtocolTests(std::vector<TestCase>& tests);
void RegisterControlSpecTests(std::vector<TestCase>& tests);
void RegisterControlChannelServiceTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellControlSettingsTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellLspSettingsTests(std::vector<TestCase>& tests);
void RegisterArchitectureInvariantsTests(std::vector<TestCase>& tests);
void RegisterSingleLineEditorTests(std::vector<TestCase>& tests);
void RegisterSingleLineEditorParityTests(std::vector<TestCase>& tests);
void RegisterPersistedStateRecordTests(std::vector<TestCase>& tests);
void RegisterPersistedRecordDumpTests(std::vector<TestCase>& tests);
void RegisterAllocationCounterTests(std::vector<TestCase>& tests);
void RegisterPerfBaselineTests(std::vector<TestCase>& tests);
void RegisterPerfHarnessIsolationTests(std::vector<TestCase>& tests);
void RegisterScenarioProcessIsolationTests(std::vector<TestCase>& tests);
void RegisterBackgroundTaskCounterTests(std::vector<TestCase>& tests);
void RegisterFileWriteGateTests(std::vector<TestCase>& tests);
void RegisterSaveFormatterPipelineTests(std::vector<TestCase>& tests);
void RegisterProcessLauncherTests(std::vector<TestCase>& tests);
void RegisterWorkspaceToolDownloaderTests(std::vector<TestCase>& tests);
void RegisterEditorEssentialsTests(std::vector<TestCase>& tests);
void RegisterEditorWordMotionTests(std::vector<TestCase>& tests);
void RegisterTextViewportMultiCaretReferenceTests(std::vector<TestCase>& tests);
void RegisterEditorRenderViewModelAllocationTests(std::vector<TestCase>& tests);
void RegisterPluginPresentationAllocationTests(std::vector<TestCase>& tests);
void RegisterPluginPathContainmentTests(std::vector<TestCase>& tests);
void RegisterEditorSnippetTests(std::vector<TestCase>& tests);
void RegisterFoldingModelTests(std::vector<TestCase>& tests);
void RegisterEditorFoldingTests(std::vector<TestCase>& tests);
void RegisterEditorEdgeCaseTests(std::vector<TestCase>& tests);
void RegisterMultiCaretRemapTests(std::vector<TestCase>& tests);
void RegisterEditorWrapNavigationPropertyTests(std::vector<TestCase>& tests);
void RegisterEditorWrapInvarianceTests(std::vector<TestCase>& tests);
void RegisterEditorMultiCaretTests(std::vector<TestCase>& tests);
void RegisterEditorMultiCaretMotionTests(std::vector<TestCase>& tests);
void RegisterBoundedResourceCapsTests(std::vector<TestCase>& tests);
void RegisterWheelAccumulatorTests(std::vector<TestCase>& tests);
void RegisterTabStripAnimationTests(std::vector<TestCase>& tests);

namespace {

void RegisterShellTests(std::vector<TestCase>& tests) {
  RegisterBoundedResourceCapsTests(tests);
  RegisterWheelAccumulatorTests(tests);
  RegisterTabStripAnimationTests(tests);
  RegisterApplicationTests(tests);
  RegisterAppStartupOptionsTests(tests);
  RegisterCompareReviewTests(tests);
  RegisterBranchReviewStateTests(tests);
  RegisterAssistServiceTests(tests);
  RegisterParityTests(tests);
  RegisterPatchApplyTests(tests);
  RegisterDiagnosticsStoreTests(tests);
  RegisterPluginDecorationStoreTests(tests);
  RegisterPluginSurfaceStoreTests(tests);
  RegisterPluginSurfacePreviewTests(tests);
  RegisterPluginDisplayListTests(tests);
  RegisterEditorRowYLayoutTests(tests);
  RegisterEditorInsetLayoutTests(tests);
  RegisterEolDecorationLayoutTests(tests);
  RegisterInlayHintColumnsTests(tests);
  RegisterBreakpointStoreTests(tests);
  RegisterDirtyRegionPolicyTests(tests);
  RegisterEditorSplitTreeTests(tests);
  RegisterTerminalPaneLayoutTests(tests);
  RegisterRecentsServiceTests(tests);
  RegisterWorkspaceShellSharedCoreTests(tests);
  RegisterWorkspaceShellSharedLayoutTests(tests);
  RegisterDebugPaneTests(tests);
  RegisterDebugPaneRenderTests(tests);
  RegisterWorkspaceShellRenderMenusTests(tests);
  RegisterWorkspaceShellRenderPromptsTests(tests);
  RegisterEditorHoverTargetTests(tests);
  RegisterEditorBracketJumpTests(tests);
  RegisterEditorDiagnosticNavigationTests(tests);
  RegisterEditorUndoRedoWiringTests(tests);
  RegisterEditorCopySurfacePriorityTests(tests);
  RegisterEditorPasteBehaviorTests(tests);
  RegisterEditorCompareSourceTests(tests);
  RegisterEditorGoToLineTests(tests);
  RegisterTabStripServiceTests(tests);
  RegisterTextDragDropTests(tests);
  RegisterSelectionGranularityTests(tests);
  RegisterSelectionGranularityPropertyTests(tests);
  RegisterMergeWrapRowsTests(tests);
  RegisterAsyncFileOpenTests(tests);
  RegisterDiffWrapLayoutTests(tests);
  RegisterImeCompositionTests(tests);
  RegisterSearchDifferentialTests(tests);
  RegisterWorkspaceShellSharedSearchTests(tests);
  RegisterWorkspaceShellSharedTerminalTests(tests);
  RegisterPluginHostTests(tests);
  RegisterPluginPureHelperTests(tests);
  RegisterPluginSurfaceCoverageTests(tests);
  RegisterSurfaceTextureCacheTests(tests);
  RegisterWorkspaceShellFormatJsonTests(tests);
  RegisterSaveDataIntegrityTests(tests);
  RegisterControlClientTests(tests);
  RegisterControlProtocolRobustnessTests(tests);
  RegisterControlProtocolTests(tests);
  RegisterControlSpecTests(tests);
  RegisterControlChannelServiceTests(tests);
  RegisterWorkspaceShellControlSettingsTests(tests);
  RegisterWorkspaceShellLspSettingsTests(tests);
  RegisterArchitectureInvariantsTests(tests);
  RegisterEditorGroupStateTests(tests);
  RegisterSingleLineEditorTests(tests);
  RegisterSingleLineEditorParityTests(tests);
  RegisterPersistedStateRecordTests(tests);
  RegisterPersistedRecordDumpTests(tests);
  RegisterAllocationCounterTests(tests);
  RegisterPerfBaselineTests(tests);
  RegisterPerfHarnessIsolationTests(tests);
  RegisterScenarioProcessIsolationTests(tests);
  RegisterTerminalWordSelectionTests(tests);
  RegisterMainThreadMailboxTests(tests);
  RegisterRenderViewModelBuilderTests(tests);
  RegisterRowDecorationBuilderTests(tests);
  RegisterTextRendererTests(tests);
  RegisterThemeTests(tests);
  RegisterTextViewportTests(tests);
  RegisterTextLayoutTests(tests);
  RegisterPieceTreeTests(tests);
  RegisterMenuAcceleratorConsistencyTests(tests);
  RegisterCommandLabelConsistencyTests(tests);
  RegisterWorkspaceMenuRegistryTests(tests);
  RegisterWorkspaceSettingsRegistryTests(tests);
  RegisterSettingsStoreTests(tests);
  RegisterWorkspaceTestControllerTests(tests);
  RegisterWorkspaceStatusBarTests(tests);
  RegisterNotificationServiceTests(tests);
  RegisterWorkspaceShellChromeTests(tests);
  RegisterWorkspaceShellEditorBlameTests(tests);
  RegisterWorkspaceShellPluginTests(tests);
  RegisterWorkspaceShellPromptTests(tests);
  RegisterWorkspaceShellCompareTests(tests);
  RegisterWorkspaceFileDropTests(tests);
  RegisterColumnSelectionTests(tests);
  RegisterWorkspaceShellRenderSurfaceTests(tests);
  RegisterWorkspaceShellCursorTests(tests);
  RegisterWorkspaceShellProjectTests(tests);
  RegisterWorkspaceShellSearchTests(tests);
  RegisterWorkspaceShellSessionTests(tests);
  RegisterWorkspaceShellSourceControlTests(tests);
  RegisterWorkspaceShellTerminalTests(tests);
  RegisterGitServiceTests(tests);
  RegisterRuntimeSyntaxSkipTests(tests);
  RegisterLineIndentScanTests(tests);
  RegisterPersistedRecordWriteQueueTests(tests);
  RegisterSyntaxDefinitionLoaderTests(tests);
  RegisterGitRepositoryServiceTests(tests);
  RegisterCommitWorkflowTests(tests);
  RegisterGitSidebarCommandCenterTests(tests);
  RegisterWorkspaceLspClientTests(tests);
  RegisterLspFileWatchTests(tests);
  RegisterLspProtocolTests(tests);
  RegisterLspResourceOpsTests(tests);
  RegisterLspPositionEncodingTests(tests);
  RegisterLspRealServerE2ETests(tests);
  RegisterDapRealAdapterE2ETests(tests);
  RegisterWorkspaceDapClientTests(tests);
  RegisterDapProtocolTests(tests);
  RegisterDebugServiceTests(tests);
  RegisterMergeModelTests(tests);
  RegisterSurfaceTokenWindowTests(tests);
  RegisterReviewTabPlanTests(tests);
  RegisterReviewSessionTests(tests);
  RegisterMergeConflictResolutionTests(tests);
  RegisterContributionRegistryTests(tests);
  RegisterPhase4Tests(tests);
  RegisterPhase5Tests(tests);
  RegisterBackgroundTaskCounterTests(tests);
  RegisterFileWriteGateTests(tests);
  RegisterSaveFormatterPipelineTests(tests);
  RegisterProcessLauncherTests(tests);
  RegisterExternalRepoChangeTests(tests);
  RegisterWorkspaceToolDownloaderTests(tests);
  RegisterEditorEssentialsTests(tests);
  RegisterEditorWordMotionTests(tests);
  RegisterTextViewportMultiCaretReferenceTests(tests);
  RegisterEditorRenderViewModelAllocationTests(tests);
  RegisterPluginPresentationAllocationTests(tests);
  RegisterPluginPathContainmentTests(tests);
  RegisterEditorSnippetTests(tests);
  RegisterFoldingModelTests(tests);
  RegisterEditorFoldingTests(tests);
  RegisterEditorEdgeCaseTests(tests);
  RegisterMultiCaretRemapTests(tests);
  RegisterEditorWrapNavigationPropertyTests(tests);
  RegisterEditorWrapInvarianceTests(tests);
  RegisterEditorMultiCaretTests(tests);
  RegisterEditorMultiCaretMotionTests(tests);
}

}  // namespace

}  // namespace microide::tests

int main(int argc, char** argv) {
  microide::tests::TestSuiteConfig config;
  config.binary_name = "microide_tests";
  config.register_tests = &microide::tests::RegisterShellTests;
  // Bind the kernel's wake path to SDL's event queue, exactly as Application does.
  // Without it every producer's wake push fails and latches the shared owed bit,
  // which is correct-but-degraded behaviour that several tests assert against.
  config.install_process_hooks = []() { microide::app::InstallSdlWaker(); };
  // Establish the dummy SDL video subsystem once, before any test runs. SDL's
  // dummy driver derives the display content scale at video-init time; if any
  // test emits an SDL_Log (e.g. the redraw-trace flush exercises production
  // logging) before video is initialized, SDL's lazy-init path yields a 2x
  // content scale. That stray scale makes the logical render size half the
  // pixel output size, which disables the retained scene-texture path and fails
  // Application/HeadlessRendersRetainedSceneFrame depending on test order.
  // Initializing up front removes the order dependence on shared global state.
  config.set_up = []() -> std::optional<std::string> {
    try {
      microide::tests::EnsureDummySdlVideoInitialized();
    } catch (const std::exception& error) {
      return std::string(error.what());
    }
    return std::nullopt;
  };
  config.tear_down = []() { SDL_Quit(); };
  return microide::tests::RunTestSuite(argc, argv, config);
}
