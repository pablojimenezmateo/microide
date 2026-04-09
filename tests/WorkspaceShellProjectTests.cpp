#include "TestSupport.h"

#include "WorkspaceShellTestAccess.h"

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using microide::workspace::WorkspaceShellTestAccess;

void TestWorkspaceShellProjectOpenMenuUsesNativePickerSelection() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "picked-project";
  const std::filesystem::path readme = root / "README.md";
  WriteFile(readme, "hello\n");

  WorkspaceShell shell;
  std::filesystem::path requested_default;
  int launch_count = 0;
  WorkspaceShellTestAccess::SetProjectOpenDialogLauncher(
      shell, [&](WorkspaceShell&, const std::filesystem::path& default_location) {
        ++launch_count;
        requested_default = default_location.lexically_normal();
        return true;
      });

  Expect(WorkspaceShellTestAccess::ExecuteProjectOpenFromMenu(shell),
         "menu open-project should launch the native picker");
  Expect(launch_count == 1, "menu open-project should launch the picker exactly once");
  Expect(!requested_default.empty(),
         "menu open-project should provide a default location to the picker");
  Expect(WorkspaceShellTestAccess::ProjectOpenDialogActive(shell),
         "menu open-project should mark the picker as active while waiting");
  Expect(!WorkspaceShellTestAccess::CommandMode(shell),
         "menu open-project should not fall back to command mode when native launch succeeds");

  WorkspaceShellTestAccess::QueueProjectOpenDialogSelection(shell, root);
  WorkspaceShellTestAccess::ConsumePendingProjectOpenDialogResult(shell);

  Expect(!WorkspaceShellTestAccess::ProjectOpenDialogActive(shell),
         "project picker should clear its active state after selection");
  Expect(WorkspaceShellTestAccess::ProjectCount(shell) == 1,
         "selected project should open as a project tab");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root.lexically_normal(),
         "selected project should become the active project");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "selected project should open its startup file");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == readme.lexically_normal(),
         "selected project should open the README startup file");
}

void TestWorkspaceShellProjectOpenCommandUsesNativePickerAtActiveProjectRoot() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "current-project";
  const std::filesystem::path readme = root / "README.md";
  WriteFile(readme, "root\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  std::filesystem::path requested_default;
  int launch_count = 0;
  WorkspaceShellTestAccess::SetProjectOpenDialogLauncher(
      shell, [&](WorkspaceShell&, const std::filesystem::path& default_location) {
        ++launch_count;
        requested_default = default_location.lexically_normal();
        return true;
      });

  Expect(WorkspaceShellTestAccess::ExecuteProjectOpenFromCommand(shell),
         "command open-project without a path should launch the native picker");
  Expect(launch_count == 1, "command open-project should launch the picker exactly once");
  Expect(requested_default == root.lexically_normal(),
         "command open-project should seed the picker from the active project root");

  WorkspaceShellTestAccess::QueueProjectOpenDialogCancel(shell);
  WorkspaceShellTestAccess::ConsumePendingProjectOpenDialogResult(shell);

  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root.lexically_normal(),
         "cancelled project picker should leave the active project unchanged");
  Expect(WorkspaceShellTestAccess::StatusMessage(shell).find("cancelled") != std::string::npos,
         "cancelled project picker should report the cancellation");
}

void TestWorkspaceShellProjectOpenMenuFallsBackToTypedPathWhenNativePickerFails() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectOpenDialogLauncher(
      shell, [](WorkspaceShell&, const std::filesystem::path&) { return false; });

  Expect(WorkspaceShellTestAccess::ExecuteProjectOpenFromMenu(shell),
         "menu open-project should stay handled when native launch fails");
  Expect(!WorkspaceShellTestAccess::ProjectOpenDialogActive(shell),
         "failed picker launch should not leave the picker marked active");
  Expect(WorkspaceShellTestAccess::CommandMode(shell),
         "menu open-project should fall back to the typed command prompt");
  Expect(WorkspaceShellTestAccess::CommandInput(shell) == "project-open ",
         "menu fallback should prefill the typed open-project command");
  Expect(WorkspaceShellTestAccess::StatusMessage(shell).find("Enter a project path") !=
             std::string::npos,
         "menu fallback should tell the user to enter a project path");
}

}  // namespace

void RegisterWorkspaceShellProjectTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/ProjectOpenMenuUsesNativePickerSelection",
          TestWorkspaceShellProjectOpenMenuUsesNativePickerSelection);
  AddTest(tests, "WorkspaceShell/ProjectOpenCommandUsesNativePickerAtActiveProjectRoot",
          TestWorkspaceShellProjectOpenCommandUsesNativePickerAtActiveProjectRoot);
  AddTest(tests, "WorkspaceShell/ProjectOpenMenuFallsBackToTypedPathWhenNativePickerFails",
          TestWorkspaceShellProjectOpenMenuFallsBackToTypedPathWhenNativePickerFails);
}

}  // namespace microide::tests
