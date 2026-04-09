#include "TestSupport.h"

#include "WorkspaceShellTestAccess.h"

#include <algorithm>
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

void TestWorkspaceShellTreeCollapseAllowsOpenDescendantsAndReselectReveal() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path readme = root / "README.md";
  const std::filesystem::path source_dir = root / "src";
  const std::filesystem::path source = source_dir / "main.cpp";
  std::filesystem::create_directories(source_dir);
  WriteFile(readme, "readme\n");
  WriteFile(source, "int main() {}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, readme);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  const auto tree_contains_path = [&](const std::filesystem::path& path) {
    const auto& entries = WorkspaceShellTestAccess::TreeEntries(shell);
    return std::any_of(entries.begin(), entries.end(),
                       [&](const auto& entry) { return entry.path == path.lexically_normal(); });
  };

  Expect(tree_contains_path(source),
         "opening a nested file should reveal it in the tree initially");
  Expect(WorkspaceShellTestAccess::SelectTreePath(shell, source_dir),
         "tree collapse fixture should be able to select the parent directory");

  WorkspaceShellTestAccess::CollapseTreeSelection(shell);

  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == source_dir.lexically_normal(),
         "collapsing an open-file ancestor should leave the directory selected");
  Expect(!tree_contains_path(source),
         "collapsing an open-file ancestor should hide the descendant rows");

  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  Expect(!tree_contains_path(source),
         "selecting another tab should not force unrelated collapsed directories open");

  WorkspaceShellTestAccess::ActivateTab(shell, 1);

  Expect(tree_contains_path(source),
         "reselecting the open file tab should re-expand its ancestors in the tree");
  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == source.lexically_normal(),
         "reselecting the open file tab should reselect the file in the tree");
}

}  // namespace

void RegisterWorkspaceShellProjectTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/ProjectOpenMenuUsesNativePickerSelection",
          TestWorkspaceShellProjectOpenMenuUsesNativePickerSelection);
  AddTest(tests, "WorkspaceShell/ProjectOpenCommandUsesNativePickerAtActiveProjectRoot",
          TestWorkspaceShellProjectOpenCommandUsesNativePickerAtActiveProjectRoot);
  AddTest(tests, "WorkspaceShell/ProjectOpenMenuFallsBackToTypedPathWhenNativePickerFails",
          TestWorkspaceShellProjectOpenMenuFallsBackToTypedPathWhenNativePickerFails);
  AddTest(tests, "WorkspaceShell/TreeCollapseAllowsOpenDescendantsAndReselectReveal",
          TestWorkspaceShellTreeCollapseAllowsOpenDescendantsAndReselectReveal);
}

}  // namespace microide::tests
