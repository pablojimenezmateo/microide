#include "TestSupport.h"
#include "TestRunnerCli.h"
#include "terminal/TerminalSession.h"
#include "util/Parse.h"
#include "util/TraceChannel.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace microide::tests {

void RegisterAppDirectoriesTests(std::vector<TestCase>& tests);
void RegisterApplicationTests(std::vector<TestCase>& tests);
void RegisterAppStartupOptionsTests(std::vector<TestCase>& tests);
void RegisterTestSupportTests(std::vector<TestCase>& tests);
void RegisterCompareModelTests(std::vector<TestCase>& tests);
void RegisterCompareReviewTests(std::vector<TestCase>& tests);
void RegisterBranchReviewStateTests(std::vector<TestCase>& tests);
void RegisterAssistServiceTests(std::vector<TestCase>& tests);
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
void RegisterDirectoryTreeTests(std::vector<TestCase>& tests);
void RegisterDirtyRegionPolicyTests(std::vector<TestCase>& tests);
void RegisterFilesystemTests(std::vector<TestCase>& tests);
void RegisterGlobMatchTests(std::vector<TestCase>& tests);
void RegisterGitBranchOperationsTests(std::vector<TestCase>& tests);
void RegisterIgnoreMatcherTests(std::vector<TestCase>& tests);
void RegisterProjectTraversalFilterTests(std::vector<TestCase>& tests);
void RegisterCompileCommandsLocatorTests(std::vector<TestCase>& tests);
void RegisterWorkspaceProjectFileMonitorTests(std::vector<TestCase>& tests);
void RegisterFileFinderTests(std::vector<TestCase>& tests);
void RegisterRecentsServiceTests(std::vector<TestCase>& tests);
void RegisterFileIndexTests(std::vector<TestCase>& tests);
void RegisterFileOperationServiceTests(std::vector<TestCase>& tests);
void RegisterGitBlameServiceTests(std::vector<TestCase>& tests);
void RegisterGitServiceTests(std::vector<TestCase>& tests);
void RegisterRuntimeSyntaxSkipTests(std::vector<TestCase>& tests);
void RegisterLineEditSpanTests(std::vector<TestCase>& tests);
void RegisterPersistedRecordWriteQueueTests(std::vector<TestCase>& tests);
void RegisterSyntaxDefinitionLoaderTests(std::vector<TestCase>& tests);
void RegisterGitRepositoryStateTests(std::vector<TestCase>& tests);
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
void RegisterMergeConflictResolutionTests(std::vector<TestCase>& tests);
void RegisterReviewTabPlanTests(std::vector<TestCase>& tests);
void RegisterReviewSessionTests(std::vector<TestCase>& tests);
void RegisterPluginHostTests(std::vector<TestCase>& tests);
void RegisterPluginSurfaceCoverageTests(std::vector<TestCase>& tests);
void RegisterSurfaceTextureCacheTests(std::vector<TestCase>& tests);
void RegisterPluginThreadTests(std::vector<TestCase>& tests);
void RegisterProjectSearchServiceTests(std::vector<TestCase>& tests);
void RegisterProjectChangeTests(std::vector<TestCase>& tests);
void RegisterExternalRepoChangeTests(std::vector<TestCase>& tests);
void RegisterRegexUtilTests(std::vector<TestCase>& tests);
void RegisterRuntimePathsTests(std::vector<TestCase>& tests);
void RegisterStringUtilTests(std::vector<TestCase>& tests);
void RegisterSha256Tests(std::vector<TestCase>& tests);
void RegisterSubprocessTests(std::vector<TestCase>& tests);
void RegisterTaskExecutorTests(std::vector<TestCase>& tests);
void RegisterMainThreadMailboxTests(std::vector<TestCase>& tests);
void RegisterSerialWorkQueueTests(std::vector<TestCase>& tests);
void RegisterWakePipeTests(std::vector<TestCase>& tests);
void RegisterGenerationTests(std::vector<TestCase>& tests);
void RegisterProjectBackgroundExecutorTests(std::vector<TestCase>& tests);
void RegisterRenderViewModelBuilderTests(std::vector<TestCase>& tests);
void RegisterRowDecorationBuilderTests(std::vector<TestCase>& tests);
void RegisterTerminalBackendTests(std::vector<TestCase>& tests);
void RegisterTerminalSessionTests(std::vector<TestCase>& tests);
void RegisterTerminalSearchTests(std::vector<TestCase>& tests);
void RegisterThemeTests(std::vector<TestCase>& tests);
void RegisterTextRendererTests(std::vector<TestCase>& tests);
void RegisterTextViewportTests(std::vector<TestCase>& tests);
void RegisterTextLayoutTests(std::vector<TestCase>& tests);
void RegisterPieceTreeTests(std::vector<TestCase>& tests);
void RegisterWindowPresentationTests(std::vector<TestCase>& tests);
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
void RegisterWorkspaceShellCursorTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellProjectTests(std::vector<TestCase>& tests);
void RegisterEditorGroupStateTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSearchTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSessionTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedCoreTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedLayoutTests(std::vector<TestCase>& tests);
void RegisterDebugPaneTests(std::vector<TestCase>& tests);
void RegisterTabStripServiceTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedSearchTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSourceControlTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedTerminalTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellTerminalTests(std::vector<TestCase>& tests);
void RegisterWorkspaceStatusBarTests(std::vector<TestCase>& tests);
void RegisterNotificationServiceTests(std::vector<TestCase>& tests);
void RegisterContributionRegistryTests(std::vector<TestCase>& tests);
void RegisterPhase3Tests(std::vector<TestCase>& tests);
void RegisterPhase4Tests(std::vector<TestCase>& tests);
void RegisterPhase5Tests(std::vector<TestCase>& tests);
void RegisterParseTests(std::vector<TestCase>& tests);
void RegisterJsonValueTests(std::vector<TestCase>& tests);
void RegisterJsonFormatTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellFormatJsonTests(std::vector<TestCase>& tests);
void RegisterProjectFileScannerTests(std::vector<TestCase>& tests);
void RegisterTextFileIOTests(std::vector<TestCase>& tests);
void RegisterSaveDataIntegrityTests(std::vector<TestCase>& tests);
void RegisterControlSocketServerTests(std::vector<TestCase>& tests);
void RegisterControlClientTests(std::vector<TestCase>& tests);
void RegisterControlProtocolTests(std::vector<TestCase>& tests);
void RegisterControlSpecTests(std::vector<TestCase>& tests);
void RegisterControlChannelServiceTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellControlSettingsTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellLspSettingsTests(std::vector<TestCase>& tests);
void RegisterArchitectureInvariantsTests(std::vector<TestCase>& tests);
void RegisterSingleLineEditorTests(std::vector<TestCase>& tests);
void RegisterPersistedRecordIoTests(std::vector<TestCase>& tests);
void RegisterPersistedStateRecordTests(std::vector<TestCase>& tests);
void RegisterPersistedRecordDumpTests(std::vector<TestCase>& tests);
void RegisterAllocationCounterTests(std::vector<TestCase>& tests);
void RegisterPerfBaselineTests(std::vector<TestCase>& tests);
void RegisterPerfHarnessIsolationTests(std::vector<TestCase>& tests);
void RegisterBackgroundTaskCounterTests(std::vector<TestCase>& tests);
void RegisterFileIndexWatcherTests(std::vector<TestCase>& tests);
void RegisterFileIndexWatcherContractTests(std::vector<TestCase>& tests);
void RegisterTerminalLifecycleStressTests(std::vector<TestCase>& tests);
void RegisterPatternCacheTests(std::vector<TestCase>& tests);
void RegisterWorkspaceToolDownloaderTests(std::vector<TestCase>& tests);
void RegisterEditorEssentialsTests(std::vector<TestCase>& tests);
void RegisterEditorRenderViewModelAllocationTests(std::vector<TestCase>& tests);
void RegisterPluginPresentationAllocationTests(std::vector<TestCase>& tests);
void RegisterPluginPathContainmentTests(std::vector<TestCase>& tests);
void RegisterEditorSnippetTests(std::vector<TestCase>& tests);
void RegisterFoldingModelTests(std::vector<TestCase>& tests);
void RegisterEditorFoldingTests(std::vector<TestCase>& tests);
void RegisterEditorMultiCaretTests(std::vector<TestCase>& tests);
void RegisterTestRunnerCliTests(std::vector<TestCase>& tests);
void RegisterBoundedResourceCapsTests(std::vector<TestCase>& tests);
void RegisterWheelAccumulatorTests(std::vector<TestCase>& tests);
void RegisterTabStripAnimationTests(std::vector<TestCase>& tests);

}  // namespace microide::tests

namespace {

bool WildcardMatch(std::string_view pattern, std::string_view text) {
  std::size_t pattern_index = 0;
  std::size_t text_index = 0;
  std::size_t star_pattern_index = std::string_view::npos;
  std::size_t star_text_index = 0;

  while (text_index < text.size()) {
    if (pattern_index < pattern.size() &&
        (pattern[pattern_index] == '?' || pattern[pattern_index] == text[text_index])) {
      ++pattern_index;
      ++text_index;
      continue;
    }
    if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
      star_pattern_index = pattern_index++;
      star_text_index = text_index;
      continue;
    }
    if (star_pattern_index != std::string_view::npos) {
      pattern_index = star_pattern_index + 1;
      text_index = ++star_text_index;
      continue;
    }
    return false;
  }

  while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
    ++pattern_index;
  }
  return pattern_index == pattern.size();
}

bool MatchesAnyPattern(std::string_view value, std::string_view patterns) {
  if (patterns.empty()) {
    return true;
  }
  std::size_t start = 0;
  while (start <= patterns.size()) {
    const std::size_t separator = patterns.find(':', start);
    const std::size_t end = separator == std::string_view::npos ? patterns.size() : separator;
    const std::string_view pattern = patterns.substr(start, end - start);
    if (!pattern.empty() && WildcardMatch(pattern, value)) {
      return true;
    }
    if (separator == std::string_view::npos) {
      break;
    }
    start = separator + 1;
  }
  return false;
}

bool MatchesGtestFilter(std::string_view test_name, std::string_view filter_expression) {
  if (filter_expression.empty()) {
    return true;
  }
  const std::size_t dash = filter_expression.find('-');
  const std::string_view includes =
      dash == std::string_view::npos ? filter_expression : filter_expression.substr(0, dash);
  const std::string_view excludes =
      dash == std::string_view::npos ? std::string_view{} : filter_expression.substr(dash + 1);
  const bool included = includes.empty() ? true : MatchesAnyPattern(test_name, includes);
  if (!included) {
    return false;
  }
  if (!excludes.empty() && MatchesAnyPattern(test_name, excludes)) {
    return false;
  }
  return true;
}

struct GtestListNameParts {
  std::string_view suite;
  std::string_view test;
};

GtestListNameParts SplitGtestListName(std::string_view test_name) {
  const std::size_t separator = test_name.find('/');
  if (separator == std::string_view::npos) {
    return {"Ungrouped", test_name};
  }
  return {test_name.substr(0, separator), test_name.substr(separator + 1)};
}

bool IsSelected(const std::string& test_name,
                const std::vector<std::string>& substring_filters,
                const std::string* gtest_filter) {
  if (gtest_filter != nullptr) {
    return MatchesGtestFilter(test_name, *gtest_filter);
  }
  if (substring_filters.empty()) {
    return true;
  }
  for (const std::string_view filter : substring_filters) {
    if (test_name.find(filter) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void ListSelectedTestsFlat(const std::vector<microide::tests::TestCase>& tests,
                           const std::vector<std::string>& substring_filters,
                           const std::string* gtest_filter) {
  for (const auto& test : tests) {
    if (!IsSelected(test.name, substring_filters, gtest_filter)) {
      continue;
    }
    std::cout << test.name << '\n';
  }
}

void ListSelectedTestsGtest(const std::vector<microide::tests::TestCase>& tests,
                            const std::vector<std::string>& substring_filters,
                            const std::string* gtest_filter) {
  std::string_view current_suite;
  bool have_suite = false;
  for (const auto& test : tests) {
    if (!IsSelected(test.name, substring_filters, gtest_filter)) {
      continue;
    }
    const GtestListNameParts parts = SplitGtestListName(test.name);
    if (!have_suite || parts.suite != current_suite) {
      current_suite = parts.suite;
      have_suite = true;
      std::cout << current_suite << ".\n";
    }
    std::cout << "  " << parts.test << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  // Tests drive the shell from this thread; marking it keeps the trace
  // summary's main-thread split meaningful under the suite.
  microide::util::MarkTracingMainThread();

  // Use in-process placeholder terminals for the whole suite instead of spawning
  // real PTY-backed shells. Formerly a compile-time MICROIDE_TESTING fork; now a
  // runtime switch so core compiles identically for the production binary.
  microide::terminal::SetUsePlaceholderTerminalsForTesting(true);

  const auto shutdown_sdl = []() { SDL_Quit(); };

  // Isolate every user-directory write (recents MRU, user config, per-project state,
  // caches) into a throwaway temp tree for the whole process, so the suite never reads or
  // pollutes the developer's real ~/.local/state, ~/.config, etc. Several WorkspaceShell
  // fixtures open temp projects that would otherwise record their /tmp roots into the real
  // recents store. These outlive every test; individual fixtures that set their own scoped
  // XDG/HOME still override-then-restore back to these. Restored + removed at process exit.
  const microide::tests::TemporaryDirectory isolated_user_root;
  const std::filesystem::path user_root = isolated_user_root.path();
  std::error_code isolate_ec;
  std::filesystem::create_directories(user_root / "state", isolate_ec);
  std::filesystem::create_directories(user_root / "config", isolate_ec);
  std::filesystem::create_directories(user_root / "data", isolate_ec);
  std::filesystem::create_directories(user_root / "cache", isolate_ec);
  const microide::tests::ScopedEnvVar isolated_xdg_state("XDG_STATE_HOME",
                                                         (user_root / "state").string());
  const microide::tests::ScopedEnvVar isolated_xdg_config("XDG_CONFIG_HOME",
                                                          (user_root / "config").string());
  const microide::tests::ScopedEnvVar isolated_xdg_data("XDG_DATA_HOME",
                                                        (user_root / "data").string());
  const microide::tests::ScopedEnvVar isolated_xdg_cache("XDG_CACHE_HOME",
                                                         (user_root / "cache").string());
  const microide::tests::ScopedEnvVar isolated_localappdata("LOCALAPPDATA",
                                                            (user_root / "state").string());
  const microide::tests::ScopedEnvVar isolated_appdata("APPDATA",
                                                       (user_root / "config").string());

  std::vector<std::string_view> args;
  args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i] != nullptr ? std::string_view(argv[i]) : std::string_view{};
    if (!arg.empty()) {
      args.push_back(arg);
    }
  }
  const auto parsed = microide::tests::ParseTestRunnerArgs(args);
  if (parsed.error.has_value()) {
    std::cerr << "microide_tests: " << *parsed.error << "\n\n";
    microide::tests::PrintTestRunnerUsage(
        std::cerr, argc > 0 && argv[0] != nullptr ? std::string_view(argv[0]) : "microide_tests");
    shutdown_sdl();
    return 2;
  }
  if (parsed.options.show_help) {
    microide::tests::PrintTestRunnerUsage(
        std::cout, argc > 0 && argv[0] != nullptr ? std::string_view(argv[0]) : "microide_tests");
    shutdown_sdl();
    return 0;
  }

  // Establish the dummy SDL video subsystem once, before any test runs. SDL's
  // dummy driver derives the display content scale at video-init time; if any
  // test emits an SDL_Log (e.g. the redraw-trace flush exercises production
  // logging) before video is initialized, SDL's lazy-init path yields a 2x
  // content scale. That stray scale makes the logical render size half the
  // pixel output size, which disables the retained scene-texture path and fails
  // Application/HeadlessRendersRetainedSceneFrame depending on test order.
  // Initializing up front removes the order dependence on shared global state.
  try {
    microide::tests::EnsureDummySdlVideoInitialized();
  } catch (const std::exception& error) {
    std::cerr << "microide_tests: " << error.what() << '\n';
    shutdown_sdl();
    return 2;
  }

  std::vector<microide::tests::TestCase> tests;
  microide::tests::RegisterTestRunnerCliTests(tests);
  microide::tests::RegisterBoundedResourceCapsTests(tests);
  microide::tests::RegisterWheelAccumulatorTests(tests);
  microide::tests::RegisterTabStripAnimationTests(tests);
  microide::tests::RegisterAppDirectoriesTests(tests);
  microide::tests::RegisterApplicationTests(tests);
  microide::tests::RegisterAppStartupOptionsTests(tests);
  microide::tests::RegisterTestSupportTests(tests);
  microide::tests::RegisterCompareModelTests(tests);
  microide::tests::RegisterCompareReviewTests(tests);
  microide::tests::RegisterBranchReviewStateTests(tests);
  microide::tests::RegisterAssistServiceTests(tests);
  microide::tests::RegisterPatchApplyTests(tests);
  microide::tests::RegisterDiagnosticsStoreTests(tests);
  microide::tests::RegisterPluginDecorationStoreTests(tests);
  microide::tests::RegisterPluginSurfaceStoreTests(tests);
  microide::tests::RegisterPluginSurfacePreviewTests(tests);
  microide::tests::RegisterPluginDisplayListTests(tests);
  microide::tests::RegisterEditorRowYLayoutTests(tests);
  microide::tests::RegisterEditorInsetLayoutTests(tests);
  microide::tests::RegisterEolDecorationLayoutTests(tests);
  microide::tests::RegisterInlayHintColumnsTests(tests);
  microide::tests::RegisterBreakpointStoreTests(tests);
  microide::tests::RegisterDirectoryTreeTests(tests);
  microide::tests::RegisterDirtyRegionPolicyTests(tests);
  microide::tests::RegisterFilesystemTests(tests);
  microide::tests::RegisterGlobMatchTests(tests);
  microide::tests::RegisterGitBranchOperationsTests(tests);
  microide::tests::RegisterIgnoreMatcherTests(tests);
  microide::tests::RegisterProjectTraversalFilterTests(tests);
  microide::tests::RegisterCompileCommandsLocatorTests(tests);
  microide::tests::RegisterWorkspaceProjectFileMonitorTests(tests);
  microide::tests::RegisterFileFinderTests(tests);
  microide::tests::RegisterRecentsServiceTests(tests);
  microide::tests::RegisterFileIndexTests(tests);
  microide::tests::RegisterWorkspaceShellSharedCoreTests(tests);
  microide::tests::RegisterWorkspaceShellSharedLayoutTests(tests);
  microide::tests::RegisterDebugPaneTests(tests);
  microide::tests::RegisterTabStripServiceTests(tests);
  microide::tests::RegisterWorkspaceShellSharedSearchTests(tests);
  microide::tests::RegisterWorkspaceShellSharedTerminalTests(tests);
  microide::tests::RegisterPluginHostTests(tests);
  microide::tests::RegisterPluginSurfaceCoverageTests(tests);
  microide::tests::RegisterSurfaceTextureCacheTests(tests);
  microide::tests::RegisterPluginThreadTests(tests);
  microide::tests::RegisterParseTests(tests);
  microide::tests::RegisterJsonValueTests(tests);
  microide::tests::RegisterJsonFormatTests(tests);
  microide::tests::RegisterWorkspaceShellFormatJsonTests(tests);
  microide::tests::RegisterProjectFileScannerTests(tests);
  microide::tests::RegisterTextFileIOTests(tests);
  microide::tests::RegisterSaveDataIntegrityTests(tests);
  microide::tests::RegisterControlSocketServerTests(tests);
  microide::tests::RegisterControlClientTests(tests);
  microide::tests::RegisterControlProtocolTests(tests);
  microide::tests::RegisterControlSpecTests(tests);
  microide::tests::RegisterControlChannelServiceTests(tests);
  microide::tests::RegisterWorkspaceShellControlSettingsTests(tests);
  microide::tests::RegisterWorkspaceShellLspSettingsTests(tests);
  microide::tests::RegisterArchitectureInvariantsTests(tests);
  microide::tests::RegisterEditorGroupStateTests(tests);
  microide::tests::RegisterSingleLineEditorTests(tests);
  microide::tests::RegisterPersistedRecordIoTests(tests);
  microide::tests::RegisterPersistedStateRecordTests(tests);
  microide::tests::RegisterPersistedRecordDumpTests(tests);
  microide::tests::RegisterAllocationCounterTests(tests);
  microide::tests::RegisterPerfBaselineTests(tests);
  microide::tests::RegisterPerfHarnessIsolationTests(tests);
  microide::tests::RegisterProjectSearchServiceTests(tests);
  microide::tests::RegisterGitBlameServiceTests(tests);
  microide::tests::RegisterTerminalBackendTests(tests);
  microide::tests::RegisterTerminalLifecycleStressTests(tests);
  microide::tests::RegisterTerminalSessionTests(tests);
  microide::tests::RegisterTerminalSearchTests(tests);
  microide::tests::RegisterRegexUtilTests(tests);
  microide::tests::RegisterRuntimePathsTests(tests);
  microide::tests::RegisterStringUtilTests(tests);
  microide::tests::RegisterSha256Tests(tests);
  microide::tests::RegisterSubprocessTests(tests);
  microide::tests::RegisterTaskExecutorTests(tests);
  microide::tests::RegisterMainThreadMailboxTests(tests);
  microide::tests::RegisterSerialWorkQueueTests(tests);
  microide::tests::RegisterWakePipeTests(tests);
  microide::tests::RegisterGenerationTests(tests);
  microide::tests::RegisterProjectBackgroundExecutorTests(tests);
  microide::tests::RegisterRenderViewModelBuilderTests(tests);
  microide::tests::RegisterRowDecorationBuilderTests(tests);
  microide::tests::RegisterTextRendererTests(tests);
  microide::tests::RegisterThemeTests(tests);
  microide::tests::RegisterTextViewportTests(tests);
  microide::tests::RegisterTextLayoutTests(tests);
  microide::tests::RegisterPieceTreeTests(tests);
  microide::tests::RegisterWindowPresentationTests(tests);
  microide::tests::RegisterMenuAcceleratorConsistencyTests(tests);
  microide::tests::RegisterCommandLabelConsistencyTests(tests);
  microide::tests::RegisterWorkspaceMenuRegistryTests(tests);
  microide::tests::RegisterWorkspaceSettingsRegistryTests(tests);
  microide::tests::RegisterSettingsStoreTests(tests);
  microide::tests::RegisterWorkspaceTestControllerTests(tests);
  microide::tests::RegisterWorkspaceStatusBarTests(tests);
  microide::tests::RegisterNotificationServiceTests(tests);
  microide::tests::RegisterWorkspaceShellChromeTests(tests);
  microide::tests::RegisterWorkspaceShellEditorBlameTests(tests);
  microide::tests::RegisterWorkspaceShellPluginTests(tests);
  microide::tests::RegisterWorkspaceShellPromptTests(tests);
  microide::tests::RegisterWorkspaceShellCompareTests(tests);
  microide::tests::RegisterWorkspaceShellCursorTests(tests);
  microide::tests::RegisterWorkspaceShellProjectTests(tests);
  microide::tests::RegisterWorkspaceShellSearchTests(tests);
  microide::tests::RegisterWorkspaceShellSessionTests(tests);
  microide::tests::RegisterWorkspaceShellSourceControlTests(tests);
  microide::tests::RegisterWorkspaceShellTerminalTests(tests);
  microide::tests::RegisterGitServiceTests(tests);
  microide::tests::RegisterRuntimeSyntaxSkipTests(tests);
  microide::tests::RegisterLineEditSpanTests(tests);
  microide::tests::RegisterPersistedRecordWriteQueueTests(tests);
  microide::tests::RegisterSyntaxDefinitionLoaderTests(tests);
  microide::tests::RegisterGitRepositoryStateTests(tests);
  microide::tests::RegisterGitRepositoryServiceTests(tests);
  microide::tests::RegisterCommitWorkflowTests(tests);
  microide::tests::RegisterGitSidebarCommandCenterTests(tests);
  microide::tests::RegisterWorkspaceLspClientTests(tests);
  microide::tests::RegisterLspFileWatchTests(tests);
  microide::tests::RegisterLspProtocolTests(tests);
  microide::tests::RegisterLspResourceOpsTests(tests);
  microide::tests::RegisterLspPositionEncodingTests(tests);
  microide::tests::RegisterLspRealServerE2ETests(tests);
  microide::tests::RegisterDapRealAdapterE2ETests(tests);
  microide::tests::RegisterWorkspaceDapClientTests(tests);
  microide::tests::RegisterDapProtocolTests(tests);
  microide::tests::RegisterDebugServiceTests(tests);
  microide::tests::RegisterMergeModelTests(tests);
  microide::tests::RegisterReviewTabPlanTests(tests);
  microide::tests::RegisterReviewSessionTests(tests);
  microide::tests::RegisterMergeConflictResolutionTests(tests);
  microide::tests::RegisterFileOperationServiceTests(tests);
  microide::tests::RegisterContributionRegistryTests(tests);
  microide::tests::RegisterPhase3Tests(tests);
  microide::tests::RegisterPhase4Tests(tests);
  microide::tests::RegisterPhase5Tests(tests);
  microide::tests::RegisterBackgroundTaskCounterTests(tests);
  microide::tests::RegisterFileIndexWatcherTests(tests);
  microide::tests::RegisterFileIndexWatcherContractTests(tests);
  microide::tests::RegisterProjectChangeTests(tests);
  microide::tests::RegisterExternalRepoChangeTests(tests);
  microide::tests::RegisterPatternCacheTests(tests);
  microide::tests::RegisterWorkspaceToolDownloaderTests(tests);
  microide::tests::RegisterEditorEssentialsTests(tests);
  microide::tests::RegisterEditorRenderViewModelAllocationTests(tests);
  microide::tests::RegisterPluginPresentationAllocationTests(tests);
  microide::tests::RegisterPluginPathContainmentTests(tests);
  microide::tests::RegisterEditorSnippetTests(tests);
  microide::tests::RegisterFoldingModelTests(tests);
  microide::tests::RegisterEditorFoldingTests(tests);
  microide::tests::RegisterEditorMultiCaretTests(tests);

  std::size_t selected_count = 0;
  for (const auto& test : tests) {
    if (!IsSelected(test.name, parsed.options.substring_filters, parsed.options.gtest_filter ? &*parsed.options.gtest_filter : nullptr)) {
      continue;
    }
    ++selected_count;
  }

  if (parsed.options.list_mode != microide::tests::TestListMode::None) {
    if (parsed.options.list_mode == microide::tests::TestListMode::Gtest) {
      ListSelectedTestsGtest(tests, parsed.options.substring_filters,
                             parsed.options.gtest_filter ? &*parsed.options.gtest_filter
                                                         : nullptr);
    } else {
      ListSelectedTestsFlat(tests, parsed.options.substring_filters,
                            parsed.options.gtest_filter ? &*parsed.options.gtest_filter
                                                        : nullptr);
    }
    shutdown_sdl();
    return selected_count == 0 ? 1 : 0;
  }

  if (selected_count == 0) {
    shutdown_sdl();
    std::cerr << "microide_tests: no tests matched the provided filters\n";
    return 1;
  }

  // Config knobs (env-driven so ctest/run-checks can tune without a rebuild):
  //   MICROIDE_TEST_TIMEOUT_MS  watchdog per-test limit; a test that exceeds it
  //                             is named and the process aborts (0 disables).
  //   MICROIDE_TEST_SLOW_MS     threshold above which a test prints a SLOW line.
  //   MICROIDE_TEST_TIMINGS     any non-empty value prints the slowest tests.
  const auto env_i64 = [](const char* name, std::int64_t fallback) -> std::int64_t {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
      return fallback;
    }
    const std::optional<std::int64_t> parsed_value = microide::util::ParseInt64(raw);
    return parsed_value.value_or(fallback);
  };
  const std::int64_t watchdog_ms = env_i64("MICROIDE_TEST_TIMEOUT_MS", 300000);
  const std::int64_t slow_ms = env_i64("MICROIDE_TEST_SLOW_MS", 2000);
  const bool print_timings =
      parsed.options.print_timings || std::getenv("MICROIDE_TEST_TIMINGS") != nullptr;

  using Clock = std::chrono::steady_clock;
  const auto to_ms = [](Clock::duration d) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
  };

  // Watchdog: names (then aborts on) any test that runs away, so a hang or a
  // pathological test surfaces as a clear diagnostic instead of a mysterious
  // whole-shard ctest timeout. It watches an atomic "current test started at"
  // stamp; the run loop clears it between tests.
  std::atomic<std::int64_t> current_test_started{0};  // 0 == no test running
  std::atomic<const std::string*> current_test_name{nullptr};
  std::atomic<bool> watchdog_stop{false};
  std::thread watchdog;
  if (watchdog_ms > 0) {
    watchdog = std::thread([&]() {
      while (!watchdog_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const std::int64_t started = current_test_started.load(std::memory_order_acquire);
        if (started == 0) {
          continue;
        }
        const std::int64_t elapsed = to_ms(Clock::now().time_since_epoch()) - started;
        if (elapsed >= watchdog_ms) {
          const std::string* name = current_test_name.load(std::memory_order_acquire);
          std::cerr << "microide_tests: TIMEOUT after " << elapsed << "ms in "
                    << (name != nullptr ? *name : std::string("<unknown>")) << '\n';
          std::cerr.flush();
          std::abort();
        }
      }
    });
  }

  // Round-robin sharding: assign every selected test a stable 0-based ordinal
  // and run only those whose ordinal lands in this shard. With the default
  // shard_count == 1 this runs the whole suite. A shard whose slice is empty
  // (shard_count > selected_count) is NOT an error — only a truly empty filter
  // selection is (handled above). See TestRunnerCliOptions.
  const int shard_index = parsed.options.shard_index;
  const auto shard_count = static_cast<std::size_t>(parsed.options.shard_count);
  std::size_t selected_ordinal = 0;
  std::size_t ran_count = 0;
  std::vector<std::pair<std::int64_t, const std::string*>> timings;
  int exit_code = 0;
  for (const auto& test : tests) {
    if (!IsSelected(test.name, parsed.options.substring_filters,
                    parsed.options.gtest_filter ? &*parsed.options.gtest_filter : nullptr)) {
      continue;
    }
    const std::size_t ordinal = selected_ordinal++;
    if (static_cast<int>(ordinal % shard_count) != shard_index) {
      continue;
    }
    ++ran_count;
    if (parsed.options.verbose) {
      std::cerr << "[" << ran_count << "] " << test.name << '\n';
    }
    current_test_name.store(&test.name, std::memory_order_release);
    current_test_started.store(to_ms(Clock::now().time_since_epoch()),
                               std::memory_order_release);
    const auto start = Clock::now();
    try {
      test.run();
    } catch (const std::exception& error) {
      std::cerr << "microide_tests failed in " << test.name << ": " << error.what() << '\n';
      exit_code = 1;
      break;
    } catch (...) {
      std::cerr << "microide_tests failed in " << test.name << ": unknown exception\n";
      exit_code = 1;
      break;
    }
    const std::int64_t elapsed_ms = to_ms(Clock::now() - start);
    current_test_started.store(0, std::memory_order_release);
    timings.emplace_back(elapsed_ms, &test.name);
    if (slow_ms > 0 && elapsed_ms >= slow_ms) {
      std::cerr << "microide_tests: SLOW " << elapsed_ms << "ms " << test.name << '\n';
    }
  }

  watchdog_stop.store(true, std::memory_order_relaxed);
  if (watchdog.joinable()) {
    watchdog.join();
  }

  if (print_timings && !timings.empty()) {
    std::stable_sort(timings.begin(), timings.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    const std::size_t show = std::min<std::size_t>(timings.size(), 25);
    std::cerr << "microide_tests: slowest " << show << " of " << timings.size() << " tests:\n";
    for (std::size_t i = 0; i < show; ++i) {
      std::cerr << "  " << timings[i].first << "ms\t" << *timings[i].second << '\n';
    }
  }

  if (exit_code != 0) {
    shutdown_sdl();
    return exit_code;
  }

  // Final summary so callers (and agents) get an unambiguous pass signal
  // without having to inspect the exit code or scrape verbose output.
  if (shard_count > 1) {
    std::cerr << "microide_tests: OK (shard " << shard_index << "/" << shard_count << ": "
              << ran_count << " of " << selected_count
              << (selected_count == 1 ? " test)\n" : " tests)\n");
  } else {
    std::cerr << "microide_tests: OK (" << selected_count
              << (selected_count == 1 ? " test passed)\n" : " tests passed)\n");
  }
  shutdown_sdl();
  return 0;
}
