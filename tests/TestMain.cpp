#include "TestSupport.h"
#include "TestRunnerCli.h"

#include <SDL3/SDL.h>

#include <exception>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace microide::tests {

void RegisterAppDirectoriesTests(std::vector<TestCase>& tests);
void RegisterApplicationTests(std::vector<TestCase>& tests);
void RegisterAppStartupOptionsTests(std::vector<TestCase>& tests);
void RegisterCompareModelTests(std::vector<TestCase>& tests);
void RegisterCompareReviewTests(std::vector<TestCase>& tests);
void RegisterBranchReviewStateTests(std::vector<TestCase>& tests);
void RegisterPatchApplyTests(std::vector<TestCase>& tests);
void RegisterDiagnosticsStoreTests(std::vector<TestCase>& tests);
void RegisterPluginDecorationStoreTests(std::vector<TestCase>& tests);
void RegisterPluginSurfaceStoreTests(std::vector<TestCase>& tests);
void RegisterPluginDisplayListTests(std::vector<TestCase>& tests);
void RegisterEditorRowYLayoutTests(std::vector<TestCase>& tests);
void RegisterEolDecorationLayoutTests(std::vector<TestCase>& tests);
void RegisterBreakpointStoreTests(std::vector<TestCase>& tests);
void RegisterDirectoryTreeTests(std::vector<TestCase>& tests);
void RegisterDirtyRegionPolicyTests(std::vector<TestCase>& tests);
void RegisterFilesystemTests(std::vector<TestCase>& tests);
void RegisterIgnoreMatcherTests(std::vector<TestCase>& tests);
void RegisterWorkspaceProjectFileMonitorTests(std::vector<TestCase>& tests);
void RegisterFileFinderTests(std::vector<TestCase>& tests);
void RegisterRecentsServiceTests(std::vector<TestCase>& tests);
void RegisterFileIndexTests(std::vector<TestCase>& tests);
void RegisterFileOperationServiceTests(std::vector<TestCase>& tests);
void RegisterGitBlameServiceTests(std::vector<TestCase>& tests);
void RegisterGitServiceTests(std::vector<TestCase>& tests);
void RegisterGitRepositoryStateTests(std::vector<TestCase>& tests);
void RegisterGitRepositoryServiceTests(std::vector<TestCase>& tests);
void RegisterCommitWorkflowTests(std::vector<TestCase>& tests);
void RegisterGitSidebarCommandCenterTests(std::vector<TestCase>& tests);
void RegisterWorkspaceLspClientTests(std::vector<TestCase>& tests);
void RegisterLspProtocolTests(std::vector<TestCase>& tests);
void RegisterWorkspaceDapClientTests(std::vector<TestCase>& tests);
void RegisterDapProtocolTests(std::vector<TestCase>& tests);
void RegisterDebugServiceTests(std::vector<TestCase>& tests);
void RegisterMergeModelTests(std::vector<TestCase>& tests);
void RegisterMergeConflictResolutionTests(std::vector<TestCase>& tests);
void RegisterReviewTabPlanTests(std::vector<TestCase>& tests);
void RegisterReviewSessionTests(std::vector<TestCase>& tests);
void RegisterPluginHostTests(std::vector<TestCase>& tests);
void RegisterProjectSearchServiceTests(std::vector<TestCase>& tests);
void RegisterProjectChangeTests(std::vector<TestCase>& tests);
void RegisterExternalRepoChangeTests(std::vector<TestCase>& tests);
void RegisterRegexUtilTests(std::vector<TestCase>& tests);
void RegisterRuntimePathsTests(std::vector<TestCase>& tests);
void RegisterStringUtilTests(std::vector<TestCase>& tests);
void RegisterSubprocessTests(std::vector<TestCase>& tests);
void RegisterTaskExecutorTests(std::vector<TestCase>& tests);
void RegisterProjectBackgroundExecutorTests(std::vector<TestCase>& tests);
void RegisterRenderViewModelBuilderTests(std::vector<TestCase>& tests);
void RegisterRowDecorationBuilderTests(std::vector<TestCase>& tests);
void RegisterTerminalSessionTests(std::vector<TestCase>& tests);
void RegisterThemeTests(std::vector<TestCase>& tests);
void RegisterTextRendererTests(std::vector<TestCase>& tests);
void RegisterTextViewportTests(std::vector<TestCase>& tests);
void RegisterWindowPresentationTests(std::vector<TestCase>& tests);
void RegisterWorkspaceMenuRegistryTests(std::vector<TestCase>& tests);
void RegisterWorkspaceSettingsRegistryTests(std::vector<TestCase>& tests);
void RegisterSettingsStoreTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellChromeTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellEditorBlameTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellPluginTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellPromptTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellCompareTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellCursorTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellProjectTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSearchTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSessionTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedCoreTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedLayoutTests(std::vector<TestCase>& tests);
void RegisterDebugPaneTests(std::vector<TestCase>& tests);
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
void RegisterControlSocketServerTests(std::vector<TestCase>& tests);
void RegisterControlClientTests(std::vector<TestCase>& tests);
void RegisterControlProtocolTests(std::vector<TestCase>& tests);
void RegisterControlSpecTests(std::vector<TestCase>& tests);
void RegisterControlChannelServiceTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellControlSettingsTests(std::vector<TestCase>& tests);
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
void RegisterPatternCacheTests(std::vector<TestCase>& tests);
void RegisterWorkspaceToolDownloaderTests(std::vector<TestCase>& tests);
void RegisterEditorEssentialsTests(std::vector<TestCase>& tests);
void RegisterEditorRenderViewModelAllocationTests(std::vector<TestCase>& tests);
void RegisterEditorSnippetTests(std::vector<TestCase>& tests);
void RegisterFoldingModelTests(std::vector<TestCase>& tests);
void RegisterEditorFoldingTests(std::vector<TestCase>& tests);
void RegisterEditorMultiCaretTests(std::vector<TestCase>& tests);
void RegisterTestRunnerCliTests(std::vector<TestCase>& tests);
void RegisterWheelAccumulatorTests(std::vector<TestCase>& tests);

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
  microide::tests::RegisterWheelAccumulatorTests(tests);
  microide::tests::RegisterAppDirectoriesTests(tests);
  microide::tests::RegisterApplicationTests(tests);
  microide::tests::RegisterAppStartupOptionsTests(tests);
  microide::tests::RegisterCompareModelTests(tests);
  microide::tests::RegisterCompareReviewTests(tests);
  microide::tests::RegisterBranchReviewStateTests(tests);
  microide::tests::RegisterPatchApplyTests(tests);
  microide::tests::RegisterDiagnosticsStoreTests(tests);
  microide::tests::RegisterPluginDecorationStoreTests(tests);
  microide::tests::RegisterPluginSurfaceStoreTests(tests);
  microide::tests::RegisterPluginDisplayListTests(tests);
  microide::tests::RegisterEditorRowYLayoutTests(tests);
  microide::tests::RegisterEolDecorationLayoutTests(tests);
  microide::tests::RegisterBreakpointStoreTests(tests);
  microide::tests::RegisterDirectoryTreeTests(tests);
  microide::tests::RegisterDirtyRegionPolicyTests(tests);
  microide::tests::RegisterFilesystemTests(tests);
  microide::tests::RegisterIgnoreMatcherTests(tests);
  microide::tests::RegisterWorkspaceProjectFileMonitorTests(tests);
  microide::tests::RegisterFileFinderTests(tests);
  microide::tests::RegisterRecentsServiceTests(tests);
  microide::tests::RegisterFileIndexTests(tests);
  microide::tests::RegisterWorkspaceShellSharedCoreTests(tests);
  microide::tests::RegisterWorkspaceShellSharedLayoutTests(tests);
  microide::tests::RegisterDebugPaneTests(tests);
  microide::tests::RegisterWorkspaceShellSharedSearchTests(tests);
  microide::tests::RegisterWorkspaceShellSharedTerminalTests(tests);
  microide::tests::RegisterPluginHostTests(tests);
  microide::tests::RegisterParseTests(tests);
  microide::tests::RegisterControlSocketServerTests(tests);
  microide::tests::RegisterControlClientTests(tests);
  microide::tests::RegisterControlProtocolTests(tests);
  microide::tests::RegisterControlSpecTests(tests);
  microide::tests::RegisterControlChannelServiceTests(tests);
  microide::tests::RegisterWorkspaceShellControlSettingsTests(tests);
  microide::tests::RegisterArchitectureInvariantsTests(tests);
  microide::tests::RegisterSingleLineEditorTests(tests);
  microide::tests::RegisterPersistedRecordIoTests(tests);
  microide::tests::RegisterPersistedStateRecordTests(tests);
  microide::tests::RegisterPersistedRecordDumpTests(tests);
  microide::tests::RegisterAllocationCounterTests(tests);
  microide::tests::RegisterPerfBaselineTests(tests);
  microide::tests::RegisterPerfHarnessIsolationTests(tests);
  microide::tests::RegisterProjectSearchServiceTests(tests);
  microide::tests::RegisterGitBlameServiceTests(tests);
  microide::tests::RegisterTerminalSessionTests(tests);
  microide::tests::RegisterRegexUtilTests(tests);
  microide::tests::RegisterRuntimePathsTests(tests);
  microide::tests::RegisterStringUtilTests(tests);
  microide::tests::RegisterSubprocessTests(tests);
  microide::tests::RegisterTaskExecutorTests(tests);
  microide::tests::RegisterProjectBackgroundExecutorTests(tests);
  microide::tests::RegisterRenderViewModelBuilderTests(tests);
  microide::tests::RegisterRowDecorationBuilderTests(tests);
  microide::tests::RegisterTextRendererTests(tests);
  microide::tests::RegisterThemeTests(tests);
  microide::tests::RegisterTextViewportTests(tests);
  microide::tests::RegisterWindowPresentationTests(tests);
  microide::tests::RegisterWorkspaceMenuRegistryTests(tests);
  microide::tests::RegisterWorkspaceSettingsRegistryTests(tests);
  microide::tests::RegisterSettingsStoreTests(tests);
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
  microide::tests::RegisterGitRepositoryStateTests(tests);
  microide::tests::RegisterGitRepositoryServiceTests(tests);
  microide::tests::RegisterCommitWorkflowTests(tests);
  microide::tests::RegisterGitSidebarCommandCenterTests(tests);
  microide::tests::RegisterWorkspaceLspClientTests(tests);
  microide::tests::RegisterLspProtocolTests(tests);
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
  microide::tests::RegisterProjectChangeTests(tests);
  microide::tests::RegisterExternalRepoChangeTests(tests);
  microide::tests::RegisterPatternCacheTests(tests);
  microide::tests::RegisterWorkspaceToolDownloaderTests(tests);
  microide::tests::RegisterEditorEssentialsTests(tests);
  microide::tests::RegisterEditorRenderViewModelAllocationTests(tests);
  microide::tests::RegisterEditorSnippetTests(tests);
  microide::tests::RegisterFoldingModelTests(tests);
  microide::tests::RegisterEditorFoldingTests(tests);
  microide::tests::RegisterEditorMultiCaretTests(tests);

  bool ran_any = false;
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

  std::size_t current_index = 0;
  for (const auto& test : tests) {
    if (!IsSelected(test.name, parsed.options.substring_filters,
                    parsed.options.gtest_filter ? &*parsed.options.gtest_filter : nullptr)) {
      continue;
    }
    ran_any = true;
    ++current_index;
    if (parsed.options.verbose) {
      std::cerr << "[" << current_index << "/" << selected_count << "] " << test.name << '\n';
    }
    try {
      test.run();
    } catch (const std::exception& error) {
      shutdown_sdl();
      std::cerr << "microide_tests failed in " << test.name << ": " << error.what() << '\n';
      return 1;
    } catch (...) {
      shutdown_sdl();
      std::cerr << "microide_tests failed in " << test.name << ": unknown exception\n";
      return 1;
    }
  }

  if (!ran_any) {
    shutdown_sdl();
    std::cerr << "microide_tests: no tests matched the provided filters\n";
    return 1;
  }

  // Final summary so callers (and agents) get an unambiguous pass signal
  // without having to inspect the exit code or scrape verbose output.
  std::cerr << "microide_tests: OK (" << selected_count
            << (selected_count == 1 ? " test passed)\n" : " tests passed)\n");
  shutdown_sdl();
  return 0;
}
