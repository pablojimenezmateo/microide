#include "TestSupport.h"

#include <exception>
#include <iostream>
#include <vector>

namespace microide::tests {

void RegisterCompareModelTests(std::vector<TestCase>& tests);
void RegisterFileOperationServiceTests(std::vector<TestCase>& tests);
void RegisterGitBlameServiceTests(std::vector<TestCase>& tests);
void RegisterGitServiceTests(std::vector<TestCase>& tests);
void RegisterMergeModelTests(std::vector<TestCase>& tests);
void RegisterProjectSearchServiceTests(std::vector<TestCase>& tests);
void RegisterRegexUtilTests(std::vector<TestCase>& tests);
void RegisterStringUtilTests(std::vector<TestCase>& tests);
void RegisterTerminalSessionTests(std::vector<TestCase>& tests);
void RegisterTextViewportTests(std::vector<TestCase>& tests);
void RegisterWindowPresentationTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellPromptTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellProjectTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSearchTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSessionTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedCoreTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedLayoutTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedSearchTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellSharedTerminalTests(std::vector<TestCase>& tests);
void RegisterWorkspaceShellTerminalTests(std::vector<TestCase>& tests);

}  // namespace microide::tests

int main() {
  std::vector<microide::tests::TestCase> tests;
  microide::tests::RegisterCompareModelTests(tests);
  microide::tests::RegisterWorkspaceShellSharedCoreTests(tests);
  microide::tests::RegisterWorkspaceShellSharedLayoutTests(tests);
  microide::tests::RegisterWorkspaceShellSharedSearchTests(tests);
  microide::tests::RegisterWorkspaceShellSharedTerminalTests(tests);
  microide::tests::RegisterProjectSearchServiceTests(tests);
  microide::tests::RegisterGitBlameServiceTests(tests);
  microide::tests::RegisterTerminalSessionTests(tests);
  microide::tests::RegisterRegexUtilTests(tests);
  microide::tests::RegisterStringUtilTests(tests);
  microide::tests::RegisterTextViewportTests(tests);
  microide::tests::RegisterWindowPresentationTests(tests);
  microide::tests::RegisterWorkspaceShellPromptTests(tests);
  microide::tests::RegisterWorkspaceShellProjectTests(tests);
  microide::tests::RegisterWorkspaceShellSearchTests(tests);
  microide::tests::RegisterWorkspaceShellSessionTests(tests);
  microide::tests::RegisterWorkspaceShellTerminalTests(tests);
  microide::tests::RegisterGitServiceTests(tests);
  microide::tests::RegisterMergeModelTests(tests);
  microide::tests::RegisterFileOperationServiceTests(tests);

  for (const auto& test : tests) {
    try {
      test.run();
    } catch (const std::exception& error) {
      std::cerr << "microide_tests failed in " << test.name << ": " << error.what() << '\n';
      return 1;
    } catch (...) {
      std::cerr << "microide_tests failed in " << test.name << ": unknown exception\n";
      return 1;
    }
  }

  return 0;
}
