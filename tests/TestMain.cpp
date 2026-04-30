#include "TestSupport.h"

#include <SDL3/SDL.h>

#include <exception>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {

void RegisterAppDirectoriesTests(std::vector<TestCase>& tests);
void RegisterCompareModelTests(std::vector<TestCase>& tests);
void RegisterDiagnosticsStoreTests(std::vector<TestCase>& tests);
void RegisterDirtyRegionPolicyTests(std::vector<TestCase>& tests);
void RegisterFilesystemTests(std::vector<TestCase>& tests);
void RegisterFileOperationServiceTests(std::vector<TestCase>& tests);
void RegisterGitBlameServiceTests(std::vector<TestCase>& tests);
void RegisterGitServiceTests(std::vector<TestCase>& tests);
void RegisterWorkspaceLspClientTests(std::vector<TestCase>& tests);
void RegisterMergeModelTests(std::vector<TestCase>& tests);
void RegisterPluginHostTests(std::vector<TestCase>& tests);
void RegisterProjectSearchServiceTests(std::vector<TestCase>& tests);
void RegisterRegexUtilTests(std::vector<TestCase>& tests);
void RegisterRuntimePathsTests(std::vector<TestCase>& tests);
void RegisterStringUtilTests(std::vector<TestCase>& tests);
void RegisterSubprocessTests(std::vector<TestCase>& tests);
void RegisterTaskExecutorTests(std::vector<TestCase>& tests);
void RegisterTerminalSessionTests(std::vector<TestCase>& tests);
void RegisterTextRendererTests(std::vector<TestCase>& tests);
void RegisterTextViewportTests(std::vector<TestCase>& tests);
void RegisterWindowPresentationTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellChromeTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellEditorBlameTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellPluginTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellPromptTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellCompareTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellProjectTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSearchTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSessionTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedCoreTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedLayoutTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedSearchTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSourceControlTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedTerminalTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellTerminalTests(std::vector<TestCase>& tests);
void RegisterContributionRegistryTests(std::vector<TestCase>& tests);
void RegisterPhase3Tests(std::vector<TestCase>& tests);
void RegisterPhase4Tests(std::vector<TestCase>& tests);
void RegisterPhase5Tests(std::vector<TestCase>& tests);
void RegisterParseTests(std::vector<TestCase>& tests);
void RegisterArchitectureInvariantsTests(std::vector<TestCase>& tests);
void RegisterSingleLineEditorTests(std::vector<TestCase>& tests);
void RegisterPersistedRecordIoTests(std::vector<TestCase>& tests);
void RegisterPersistedStateRecordTests(std::vector<TestCase>& tests);
void RegisterPersistedRecordDumpTests(std::vector<TestCase>& tests);
void RegisterAllocationCounterTests(std::vector<TestCase>& tests);
void RegisterPerfBaselineTests(std::vector<TestCase>& tests);

}  // namespace microide::tests

int main(int argc, char** argv) {
  const auto shutdown_sdl = []() { SDL_Quit(); };
  bool verbose = false;
  std::vector<std::string_view> filters;
  filters.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i] != nullptr ? std::string_view(argv[i]) : std::string_view{};
    if (arg == "--verbose") {
      verbose = true;
      continue;
    }
    if (!arg.empty()) {
      filters.push_back(arg);
    }
  }

  std::vector<microide::tests::TestCase> tests;
  microide::tests::RegisterAppDirectoriesTests(tests);
  microide::tests::RegisterCompareModelTests(tests);
  microide::tests::RegisterDiagnosticsStoreTests(tests);
  microide::tests::RegisterDirtyRegionPolicyTests(tests);
  microide::tests::RegisterFilesystemTests(tests);
  microide::tests::RegisterWorkspaceShellSharedCoreTests(tests);
  microide::tests::RegisterWorkspaceShellSharedLayoutTests(tests);
  microide::tests::RegisterWorkspaceShellSharedSearchTests(tests);
  microide::tests::RegisterWorkspaceShellSharedTerminalTests(tests);
  microide::tests::RegisterPluginHostTests(tests);
  microide::tests::RegisterParseTests(tests);
  microide::tests::RegisterArchitectureInvariantsTests(tests);
  microide::tests::RegisterSingleLineEditorTests(tests);
  microide::tests::RegisterPersistedRecordIoTests(tests);
  microide::tests::RegisterPersistedStateRecordTests(tests);
  microide::tests::RegisterPersistedRecordDumpTests(tests);
  microide::tests::RegisterAllocationCounterTests(tests);
  microide::tests::RegisterPerfBaselineTests(tests);
  microide::tests::RegisterProjectSearchServiceTests(tests);
  microide::tests::RegisterGitBlameServiceTests(tests);
  microide::tests::RegisterTerminalSessionTests(tests);
  microide::tests::RegisterRegexUtilTests(tests);
  microide::tests::RegisterRuntimePathsTests(tests);
  microide::tests::RegisterStringUtilTests(tests);
  microide::tests::RegisterSubprocessTests(tests);
  microide::tests::RegisterTaskExecutorTests(tests);
  microide::tests::RegisterTextRendererTests(tests);
  microide::tests::RegisterTextViewportTests(tests);
  microide::tests::RegisterWindowPresentationTests(tests);
  microide::tests::RegisterWorkspaceShellChromeTests(tests);
  microide::tests::RegisterWorkspaceShellEditorBlameTests(tests);
  microide::tests::RegisterWorkspaceShellPluginTests(tests);
  microide::tests::RegisterWorkspaceShellPromptTests(tests);
  microide::tests::RegisterWorkspaceShellCompareTests(tests);
  microide::tests::RegisterWorkspaceShellProjectTests(tests);
  microide::tests::RegisterWorkspaceShellSearchTests(tests);
  microide::tests::RegisterWorkspaceShellSessionTests(tests);
  microide::tests::RegisterWorkspaceShellSourceControlTests(tests);
  microide::tests::RegisterWorkspaceShellTerminalTests(tests);
  microide::tests::RegisterGitServiceTests(tests);
  microide::tests::RegisterWorkspaceLspClientTests(tests);
  microide::tests::RegisterMergeModelTests(tests);
  microide::tests::RegisterFileOperationServiceTests(tests);
  microide::tests::RegisterContributionRegistryTests(tests);
  microide::tests::RegisterPhase3Tests(tests);
  microide::tests::RegisterPhase4Tests(tests);
  microide::tests::RegisterPhase5Tests(tests);

  bool ran_any = false;
  std::size_t selected_count = 0;
  for (const auto& test : tests) {
    if (!filters.empty()) {
      bool selected = false;
      for (const std::string_view filter : filters) {
        if (test.name.find(filter) != std::string::npos) {
          selected = true;
          break;
        }
      }
      if (!selected) {
        continue;
      }
    }
    ++selected_count;
  }

  std::size_t current_index = 0;
  for (const auto& test : tests) {
    if (!filters.empty()) {
      bool selected = false;
      for (const std::string_view filter : filters) {
        if (test.name.find(filter) != std::string::npos) {
          selected = true;
          break;
        }
      }
      if (!selected) {
        continue;
      }
    }
    ran_any = true;
    ++current_index;
    if (verbose) {
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

  shutdown_sdl();
  return 0;
}
