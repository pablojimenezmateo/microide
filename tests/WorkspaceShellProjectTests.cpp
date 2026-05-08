#include "TestSupport.h"

#include "workspace/WorkspaceShellTestAccess.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "WorkspaceShellEventHelpers.h"

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

bool ExecuteCommand(WorkspaceShell& shell, std::string_view command) {
  return SendKeyDown(shell, SDLK_E, SDL_KMOD_CTRL) &&
         WorkspaceShellTestAccess::HandleTextInput(shell, command) &&
         SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE);
}

bool WaitForProjectReload(WorkspaceShell& shell, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool WaitForFileIndexPath(WorkspaceShell& shell,
                          const std::filesystem::path& relative_path,
                          bool expected_present,
                          std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (WorkspaceShellTestAccess::FileIndexContainsPath(shell, relative_path) == expected_present) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return WorkspaceShellTestAccess::FileIndexContainsPath(shell, relative_path) == expected_present;
}

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
}

void TestWorkspaceShellProjectOpenDefersGitSidebarRefreshUntilShown() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int value() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add startup git sidebar fixture", "startup git sidebar fixture");
  WriteFile(source, "int value() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project fixture should open");
  Expect(WorkspaceShellTestAccess::GitSidebarEntries(shell).empty(),
         "opening a project should not eagerly collect git sidebar entries");

  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "showing git sidebar should enter the refreshing state immediately");
  const auto git_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < git_deadline &&
         WorkspaceShellTestAccess::GitSidebarRefreshing(shell)) {
    WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(!WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "git sidebar should leave refreshing state once async status arrives");
  const auto& entries = WorkspaceShellTestAccess::GitSidebarEntries(shell);
  Expect(!entries.empty(),
         "showing git sidebar should render entries on wake after refresh dispatch");
  const bool found_modified_source = std::any_of(
      entries.begin(), entries.end(), [&](const WorkspaceShell::GitSidebarEntry& entry) {
        return entry.section == WorkspaceShell::GitSidebarEntry::Section::Modified &&
               entry.path == source.lexically_normal();
      });
  Expect(found_modified_source,
         "on-demand git sidebar refresh should include the modified file");
}

void TestWorkspaceShellGitSidebarRefreshDispatchIsNonBlocking() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::vector<std::filesystem::path> files;
  files.reserve(300);
  for (int i = 0; i < 300; ++i) {
    const std::filesystem::path path = root / "src" / ("file_" + std::to_string(i) + ".cpp");
    WriteFile(path, "int value() {\n  return 1;\n}\n");
    files.push_back(path);
  }

  InitializeGitRepo(root);
  CommitAll(root, "Add async git status fixture", "async git status fixture");
  for (int i = 0; i < 120; ++i) {
    WriteFile(files[static_cast<std::size_t>(i)], "int value() {\n  return 2;\n}\n");
  }

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "non-blocking git refresh fixture should open");

  const auto start = std::chrono::steady_clock::now();
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
          .count();
  Expect(elapsed < 250,
         "activating the git sidebar should dispatch status refresh without blocking the main thread");
  Expect(WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "activating git sidebar should transition immediately into refreshing state");
}

void TestWorkspaceShellProjectSwitchDiscardsStaleGitSidebarRefreshResult() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path old_root = temp_dir.path() / "old-project";
  const std::filesystem::path new_root = temp_dir.path() / "new-project";
  const std::filesystem::path old_file = old_root / "src" / "old_only.cpp";
  WriteFile(old_file, "int value() {\n  return 1;\n}\n");
  WriteFile(new_root / "src" / "new_only.cpp", "int fresh() {\n  return 3;\n}\n");

  InitializeGitRepo(old_root);
  CommitAll(old_root, "Add old project baseline", "old project baseline");
  WriteFile(old_file, "int value() {\n  return 2;\n}\n");

  InitializeGitRepo(new_root);
  CommitAll(new_root, "Add new project baseline", "new project baseline");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, old_root, false, false),
         "old project should open for stale-refresh discard fixture");
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "old project git sidebar should enter refreshing state");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, new_root, false, false),
         "switching to a new project should succeed");
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "new project git sidebar should also enter refreshing state");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline &&
         WorkspaceShellTestAccess::GitSidebarRefreshing(shell)) {
    WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(!WorkspaceShellTestAccess::GitSidebarRefreshing(shell),
         "new project git sidebar refresh should settle after project switch");

  const auto& entries = WorkspaceShellTestAccess::GitSidebarEntries(shell);
  const bool contains_old_entry =
      std::any_of(entries.begin(), entries.end(),
                  [&](const WorkspaceShell::GitSidebarEntry& entry) {
                    return entry.path == old_file.lexically_normal();
                  });
  Expect(!contains_old_entry,
         "stale git refresh results from the previous project should be discarded after switch");
}

void TestWorkspaceShellUnknownCommandKeepsPromptOpenWithFeedback() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::ResetProjectScopedState(shell, true);

  Expect(SendKeyDown(shell, SDLK_E, SDL_KMOD_CTRL),
         "Ctrl+E should open the command prompt");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "bogus-command"),
         "text input should populate the command prompt");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should attempt to execute the typed command");

  Expect(WorkspaceShellTestAccess::CommandMode(shell),
         "unknown commands should keep the command prompt open");
  Expect(WorkspaceShellTestAccess::CommandPromptStatusText(shell) == "Unknown command: bogus-command",
         "unknown commands should report an explicit prompt error");
}

void TestWorkspaceShellLeftCtrlShortcutOpensCommandPrompt() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::ResetProjectScopedState(shell, true);

  Expect(SendKeyDown(shell, SDLK_E, SDL_KMOD_LCTRL),
         "left-control Ctrl+E should open the command prompt");
  Expect(WorkspaceShellTestAccess::CommandMode(shell),
         "left-control Ctrl+E should enter command mode");
}

void TestWorkspaceShellCommandReportsMissingProjectInsteadOfSilentNoOp() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::ResetProjectScopedState(shell, true);

  Expect(SendKeyDown(shell, SDLK_E, SDL_KMOD_CTRL),
         "Ctrl+E should open the command prompt before the missing-project test");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "search"),
         "text input should populate the missing-project command");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should attempt the missing-project command");

  Expect(WorkspaceShellTestAccess::CommandMode(shell),
         "project-dependent command failures should keep the prompt open");
  Expect(WorkspaceShellTestAccess::CommandPromptStatusText(shell) == "No active project",
         "project-dependent command failures should report the missing project");
}

void TestWorkspaceShellOpenCommandRequiresPath() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  Expect(SendKeyDown(shell, SDLK_E, SDL_KMOD_CTRL),
         "Ctrl+E should open the command prompt before the open-path test");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "open"),
         "text input should populate the open command");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should attempt the open command");

  Expect(WorkspaceShellTestAccess::CommandMode(shell),
         "open without a path should keep the prompt open");
  Expect(WorkspaceShellTestAccess::CommandPromptStatusText(shell) == "open requires a path",
         "open without a path should report the missing path explicitly");
}

void TestWorkspaceShellProjectNextAndPrevCommandsCycleProjects() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  const std::filesystem::path root_c = temp_dir.path() / "gamma-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");
  WriteFile(root_c / "README.md", "gamma\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_c, false, false),
         "third project should open");

  Expect(SendKeyDown(shell, SDLK_E, SDL_KMOD_CTRL),
         "Ctrl+E should open the command prompt before cycling projects");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "project-prev"),
         "text input should populate the project-prev command");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should execute the project-prev command");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_b.lexically_normal(),
         "project-prev should activate the previous project tab");

  Expect(SendKeyDown(shell, SDLK_E, SDL_KMOD_CTRL),
         "Ctrl+E should reopen the command prompt for project-next");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "project-next"),
         "text input should populate the project-next command");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should execute the project-next command");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_c.lexically_normal(),
         "project-next should activate the next project tab");
}

void TestWorkspaceShellProjectSwitchPreservesProjectScopedCommandState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  Expect(ExecuteCommand(shell, "soft-tabs on"),
         "first project should accept a project-scoped command");
  Expect(WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "first project should persist its editor preferences after the command");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  Expect(!WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "second project should start from default editor preferences");
  Expect(ExecuteCommand(shell, "soft-tabs off"),
         "second project should accept its own project-scoped command");
  Expect(!WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "second project should keep its own editor preferences");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "switching back to the first project should succeed");
  Expect(WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "switching back should restore the first project's editor preferences");
  Expect(SendKeyDown(shell, SDLK_E, SDL_KMOD_CTRL),
         "Ctrl+E should open the command prompt after switching back");
  Expect(SendKeyDown(shell, SDLK_UP, SDL_KMOD_NONE),
         "up should recall command history for the restored first project");
  Expect(WorkspaceShellTestAccess::CommandInput(shell) == "soft-tabs on",
         "switching back should restore the first project's command history");
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "escape should dismiss the recalled first-project command prompt");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 1, false),
         "switching to the second project should succeed");
  Expect(!WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "switching forward should restore the second project's editor preferences");
  Expect(SendKeyDown(shell, SDLK_E, SDL_KMOD_CTRL),
         "Ctrl+E should open the command prompt after switching forward");
  Expect(SendKeyDown(shell, SDLK_UP, SDL_KMOD_NONE),
         "up should recall command history for the restored second project");
  Expect(WorkspaceShellTestAccess::CommandInput(shell) == "soft-tabs off",
         "switching forward should restore the second project's command history");
}

void TestWorkspaceShellProjectSwitchPreservesSearchSidebarSurfaceState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  Expect(ExecuteCommand(shell, "sidebar-width 420"),
         "first project should accept a sidebar width command");
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha query", false);
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Search,
         "first project should show the search sidebar");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "search",
         "first project should keep the search sidebar view id");
  Expect(std::fabs(WorkspaceShellTestAccess::SidebarWidth(shell) - 420.0f) < 0.001f,
         "first project should keep its sidebar width");
  Expect(WorkspaceShellTestAccess::ProjectSearchQuery(shell) == "alpha query",
         "first project should keep its project-search query");

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  Expect(ExecuteCommand(shell, "sidebar-width 320"),
         "second project should accept its own sidebar width command");
  WorkspaceShellTestAccess::ShowProblemsSidebar(shell);
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Problems,
         "second project should show its own sidebar mode");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "problems",
         "second project should keep the problems sidebar view id");
  Expect(std::fabs(WorkspaceShellTestAccess::SidebarWidth(shell) - 320.0f) < 0.001f,
         "second project should keep its sidebar width");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "switching back to the first project should succeed");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Search,
         "switching back should restore the first project's search sidebar");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "search",
         "switching back should restore the first project's sidebar view id");
  Expect(std::fabs(WorkspaceShellTestAccess::SidebarWidth(shell) - 420.0f) < 0.001f,
         "switching back should restore the first project's sidebar width");
  Expect(WorkspaceShellTestAccess::ProjectSearchQuery(shell) == "alpha query",
         "switching back should restore the first project's search query");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 1, false),
         "switching forward to the second project should succeed");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Problems,
         "switching forward should restore the second project's sidebar mode");
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == "problems",
         "switching forward should restore the second project's sidebar view id");
  Expect(std::fabs(WorkspaceShellTestAccess::SidebarWidth(shell) - 320.0f) < 0.001f,
         "switching forward should restore the second project's sidebar width");
}

void TestWorkspaceShellProjectSwitchClearsTransientInteractionState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  WorkspaceShellTestAccess::SetTransientDragTargetSidebarDivider(shell);
  WorkspaceShellTestAccess::SetTransientMouseSelecting(shell, true);

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  Expect(WorkspaceShellTestAccess::TransientDragTargetIsNone(shell),
         "opening a different project should clear the transient drag target");
  Expect(!WorkspaceShellTestAccess::TransientMouseSelecting(shell),
         "opening a different project should clear transient selection tracking");

  WorkspaceShellTestAccess::SetTransientDragTargetBottomPanelScrollbar(shell);
  WorkspaceShellTestAccess::SetTransientMouseSelecting(shell, true);

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "switching back to the first project should succeed");
  Expect(WorkspaceShellTestAccess::TransientDragTargetIsNone(shell),
         "switching back should not restore stale drag state from the previous project");
  Expect(!WorkspaceShellTestAccess::TransientMouseSelecting(shell),
         "switching back should not restore stale transient selection state");
}

void TestWorkspaceShellProjectOpenShowsDefaultTerminalPanel() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "project\n");
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config";
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "auto-terminal visibility fixture should open the project");
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::Terminal,
         "opening a project should surface the terminal panel");
  Expect(WorkspaceShellTestAccess::TerminalLaunchLabels(shell).size() == 1,
         "opening a project should prepare one default terminal tab");
}

void TestWorkspaceShellTermCommandRequestsBottomPanelRedraw() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "project\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "term redraw fixture should open the project");
  (void)shell.ConsumePendingRenderInvalidation();
  const std::size_t tab_count_before = WorkspaceShellTestAccess::TerminalLaunchLabels(shell).size();

  Expect(ExecuteCommand(shell, "term"),
         "term command should execute");
  (void)shell.ConsumePendingRenderInvalidation();
  Expect(WorkspaceShellTestAccess::PanelContent(shell) == WorkspaceShell::PanelContentKind::Terminal,
         "term command should keep the terminal panel visible");
  Expect(WorkspaceShellTestAccess::TerminalLaunchLabels(shell).size() == tab_count_before + 1,
         "term command should open exactly one additional terminal tab");
}

void TestWorkspaceShellProjectOpenDefersProjectWatcherArming() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "project\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "watcher deferral fixture should open the project");

  const auto next_delay = WorkspaceShellTestAccess::ProjectFileMonitorNextPollDelay(shell);
  Expect(!next_delay.has_value() || *next_delay != std::chrono::milliseconds(1),
         "cold project open should not expose the old synthetic 1ms project-watcher rearm tick");
}

void TestWorkspaceShellSidebarWidthCommandParsesTypedRequests() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(ExecuteCommand(shell, "sidebar-width 420"),
         "sidebar-width command should execute with a numeric width");
  Expect(std::fabs(WorkspaceShellTestAccess::SidebarWidth(shell) - 420.0f) < 0.001f,
         "sidebar-width command should apply the parsed width");

  Expect(ExecuteCommand(shell, "sidebar-width wide"),
         "sidebar-width command should still route through the command prompt");
  Expect(WorkspaceShellTestAccess::CommandMode(shell),
         "invalid sidebar-width input should keep the command prompt open");
  Expect(WorkspaceShellTestAccess::CommandPromptStatusText(shell) ==
             "sidebar-width requires a numeric width",
         "invalid sidebar-width input should report the parser failure");
}

void TestWorkspaceShellMergeCommandResolvesRelativePaths() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.cpp";
  const std::filesystem::path incoming = root / "incoming.cpp";
  const std::filesystem::path current = root / "current.cpp";
  WriteFile(base, "int value() { return 0; }\n");
  WriteFile(incoming, "int value() { return 1; }\n");
  WriteFile(current, "int value() { return 2; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  Expect(ExecuteCommand(shell, "merge base.cpp incoming.cpp current.cpp"),
         "merge command should execute for project-relative paths");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "merge command should open a merge tab");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(merge.base_path == base.lexically_normal(),
         "merge command should resolve the base path relative to the active project");
  Expect(merge.incoming_path == incoming.lexically_normal(),
         "merge command should resolve the incoming path relative to the active project");
  Expect(merge.current_path == current.lexically_normal(),
         "merge command should resolve the current path relative to the active project");
  Expect(merge.output_path == current.lexically_normal(),
         "merge command should default the output path to the current file");
}

void TestWorkspaceShellTabMoveCommandSupportsRelativeOffsets() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.cpp";
  const std::filesystem::path beta = root / "beta.cpp";
  const std::filesystem::path gamma = root / "gamma.cpp";
  WriteFile(alpha, "int alpha() { return 1; }\n");
  WriteFile(beta, "int beta() { return 2; }\n");
  WriteFile(gamma, "int gamma() { return 3; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, alpha), "first tab should open");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, beta), "second tab should open");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, gamma), "third tab should open");

  Expect(ExecuteCommand(shell, "tabmove -2"),
         "tabmove should execute with a relative offset");

  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(shell);
  Expect(tabs.size() == 3, "tabmove should keep the same tab count");
  Expect(tabs[0].path == gamma.lexically_normal() && tabs[1].path == alpha.lexically_normal() &&
             tabs[2].path == beta.lexically_normal(),
         "relative tabmove should reorder the active tab into the requested slot");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 0,
         "relative tabmove should keep the moved tab active");
}

void TestWorkspaceShellGotoAndJumpCommandsUseTypedNavigationRequests() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "line1\nline2\nline3\nline4\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  Expect(ExecuteCommand(shell, "goto 3:2"), "goto should execute with an explicit line and column");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 2 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 1,
         "goto should move the cursor to the parsed absolute location");

  Expect(ExecuteCommand(shell, "jump -1:1"),
         "jump should execute with a relative line delta and column");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 1 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 0,
         "jump should move the cursor relative to the current line");
}

void TestWorkspaceShellGotoTargetsActiveSplitViewport() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path left = root / "left.cpp";
  const std::filesystem::path right = root / "right.cpp";
  WriteFile(left, "left-1\nleft-2\nleft-3\n");
  WriteFile(right, "right-1\nright-2\nright-3\nright-4\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, left);
  Expect(WorkspaceShellTestAccess::SplitActiveEditor(shell),
         "split-editor goto fixture should create the second pane");
  Expect(WorkspaceShellTestAccess::ReplaceActiveEditorWithFile(shell, right),
         "split-editor goto fixture should replace the active pane");
  Expect(WorkspaceShellTestAccess::ActivateOrderedEditorSplit(shell, 0),
         "split-editor goto fixture should revisit the left pane");
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);
  Expect(WorkspaceShellTestAccess::ActivateOrderedEditorSplit(shell, 1),
         "split-editor goto fixture should reactivate the right pane");

  Expect(ExecuteCommand(shell, "goto 4:1"),
         "goto should execute against the active split viewport");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == right.lexically_normal(),
         "goto should keep the right split active");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 3 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 0,
         "goto should move the active split cursor instead of a stale editor copy");

  Expect(WorkspaceShellTestAccess::ActivateOrderedEditorSplit(shell, 0),
         "split-editor goto fixture should allow verifying the inactive pane");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == left.lexically_normal(),
         "the left split should still reference the original file");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 0 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 0,
         "goto should not mutate the inactive split viewport");
}

void TestWorkspaceShellGlobalCommandsApplyTypedRequests() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);

  Expect(ExecuteCommand(shell, "ui-scale 125%"),
         "ui-scale should execute with a parsed numeric scale");
  Expect(std::fabs(shell.UiScale() - 1.25f) < 0.001f,
         "ui-scale should apply the parsed scale");

  Expect(ExecuteCommand(shell, "soft-tabs on"),
         "soft-tabs should execute with a typed boolean request");
  Expect(WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "soft-tabs should enable soft tabs for editor preferences");

  Expect(ExecuteCommand(shell, "wrap on"),
         "wrap should execute with a typed boolean request");
  Expect(WorkspaceShellTestAccess::SoftWrapEnabled(shell),
         "wrap should enable soft wrap for editor preferences");

  Expect(ExecuteCommand(shell, "wrap"),
         "wrap should toggle when invoked without explicit args");
  Expect(!WorkspaceShellTestAccess::SoftWrapEnabled(shell),
         "wrap without args should invert the current soft-wrap setting");

  Expect(ExecuteCommand(shell, "focus panel"),
         "focus should execute with a typed focus target");
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell),
         "focus panel should move focus to the bottom panel when available");
}

void TestWorkspaceShellCommandPromptCompletionAndHistory() {
  WorkspaceShell shell;

  Expect(SendKeyDown(shell, SDLK_E, SDL_KMOD_CTRL),
         "Ctrl+E should open the command prompt before completion");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "soft"),
         "text input should populate the command prompt before completion");
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE),
         "tab should trigger command completion");
  Expect(WorkspaceShellTestAccess::CommandInput(shell) == "soft-tabs ",
         "tab completion should expand the unique built-in command name");
  Expect(WorkspaceShellTestAccess::CommandPromptStatusText(shell) == "Completed soft-tabs",
         "tab completion should report the completed command name");

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "on"),
         "completion fixture should allow finishing the completed command");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "enter should execute the completed command");
  Expect(!WorkspaceShellTestAccess::CommandMode(shell),
         "successful command execution should close the command prompt");

  Expect(SendKeyDown(shell, SDLK_E, SDL_KMOD_CTRL),
         "Ctrl+E should reopen the command prompt before history recall");
  Expect(SendKeyDown(shell, SDLK_UP, SDL_KMOD_NONE),
         "up should recall the previous command from history");
  Expect(WorkspaceShellTestAccess::CommandInput(shell) == "soft-tabs on",
         "history recall should restore the last executed command");
  Expect(WorkspaceShellTestAccess::CommandPromptStatusText(shell).find("History 1 / 1") !=
             std::string::npos,
         "history recall should report the active history position");

  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE),
         "down should restore the pending empty command input");
  Expect(WorkspaceShellTestAccess::CommandInput(shell).empty(),
         "history navigation back to the pending input should restore an empty prompt");

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "wr"),
         "completion fixture should allow typing a second command prefix");
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE),
         "tab should complete the wrap command");
  Expect(WorkspaceShellTestAccess::CommandInput(shell) == "wrap ",
         "tab completion should expand the wrap command name");
  Expect(WorkspaceShellTestAccess::CommandPromptStatusText(shell) == "Completed wrap",
         "tab completion should report the wrap command completion");
}

void TestWorkspaceShellCtrlNOpensUntitledTab() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  Expect(SendKeyDown(shell, SDLK_N, SDL_KMOD_CTRL),
         "Ctrl+N should be handled globally");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "Ctrl+N should open a single untitled editor tab");
  const auto& tab = WorkspaceShellTestAccess::OpenTabs(shell).front();
  Expect(tab.path.empty() && tab.title == "untitled",
         "Ctrl+N should create an untitled editor tab");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 0,
         "Ctrl+N should activate the newly opened untitled tab");
}

void TestWorkspaceShellFilesShortcutEscapeRestoresSidebarFocus() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "files shortcut should open the overlay");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell),
         "files shortcut should mark the overlay visible");
  Expect(WorkspaceShellTestAccess::OverlayModeIsFileFinder(shell),
         "files shortcut should open the file-finder overlay");
  Expect(WorkspaceShellTestAccess::FocusIsOverlay(shell),
         "files shortcut should focus the overlay");

  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "Escape should close the overlay when it is visible");
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
         "Escape should dismiss the visible overlay");
  Expect(WorkspaceShellTestAccess::FocusIsSidebar(shell),
         "closing the overlay with a visible sidebar should restore sidebar focus");
}

void TestWorkspaceShellFilesShortcutEscapeRestoresEditorFocusOnWelcome() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::ResetProjectScopedState(shell, true);

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "files shortcut should still open the overlay on the welcome surface");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell),
         "welcome files shortcut should mark the overlay visible");
  Expect(WorkspaceShellTestAccess::FocusIsOverlay(shell),
         "welcome files shortcut should focus the overlay");

  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "Escape should close the welcome overlay");
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
         "Escape should dismiss the welcome overlay");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell),
         "closing the welcome overlay should restore editor focus when no sidebar is visible");
}

void TestWorkspaceShellFilesShortcutOpensMatchedFileAfterDeferredIndexCacheBuild() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path readme = root / "README.md";
  const std::filesystem::path target = root / "src" / "target-match.cpp";
  WriteFile(readme, "hello\n");
  WriteFile(target, "int target() {\n  return 42;\n}\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project fixture should open");
  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "files shortcut should open the file finder overlay");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "target-match"),
         "typing in the file finder should be handled");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "pressing enter in the file finder should open the selected match");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == target.lexically_normal(),
         "file finder should still open matches after deferred index cache build");
}

void TestWorkspaceShellProjectOpenFromWelcomeInvalidatesCachedLayout() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::RenderFrame(shell);
  WorkspaceShellTestAccess::ResetPrepareFrameLayoutComputeCount(shell);

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project should open from welcome state");
  const auto redraw = shell.ConsumePendingRenderInvalidation();
  Expect(redraw.full || !redraw.rects.empty(),
         "opening a project from welcome should invalidate cached layout and request redraw");
}

void TestWorkspaceShellOverlayOutsideClickRestoresPrimaryFocus() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "files shortcut should open the overlay before the outside click test");
  const auto layout = microide::workspace::ComputeLayout(1280.0f, 720.0f, true, false, 288.0f, 184.0f);
  const float click_x = layout.editor_area.x + 12.0f;
  const float click_y = layout.editor_area.y + 12.0f;
  Expect(SendMouseDown(shell, click_x, click_y, SDL_BUTTON_LEFT),
         "clicking outside the overlay should be handled");
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
         "outside clicks should dismiss the overlay");
  Expect(WorkspaceShellTestAccess::FocusIsSidebar(shell),
         "outside-click overlay dismissal should restore the primary sidebar focus");
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

  Expect(!tree_contains_path(source),
         "reselecting the open file tab should preserve collapsed ancestors in the tree");
}

void TestWorkspaceShellTreeScrollDoesNotSnapToSelectionDuringRender() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  for (int i = 0; i < 40; ++i) {
    WriteFile(root / ("file" + std::to_string(i) + ".txt"), "line\n");
  }

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 320);

  const auto layout = microide::workspace::ComputeLayout(
      1280.0f, 320.0f, true, false, 288.0f, 184.0f);
  const float wheel_x = layout.sidebar.x + layout.sidebar.w * 0.5f;
  const float wheel_y = layout.sidebar.y + 72.0f;

  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == root.lexically_normal(),
         "tree scroll fixture should start with the root selected");
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, -8),
         "scrolling the tree sidebar should be handled");
  Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) > 0,
         "tree scrolling should move the sidebar away from the selected root row");

  WorkspaceShellTestAccess::RenderFrame(shell);

  Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) > 0,
         "rendering should not snap tree scrolling back to keep the selected row visible");
}

void TestWorkspaceShellTreeCollapseButtonCollapsesAllOpenDirectories() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path nested_dir = root / "src" / "nested";
  const std::filesystem::path source = nested_dir / "main.cpp";
  std::filesystem::create_directories(nested_dir);
  WriteFile(source, "int main() {}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  const auto tree_contains_path = [&](const std::filesystem::path& path) {
    const auto& entries = WorkspaceShellTestAccess::TreeEntries(shell);
    return std::any_of(entries.begin(), entries.end(),
                       [&](const auto& entry) { return entry.path == path.lexically_normal(); });
  };

  Expect(tree_contains_path(source),
         "opening a nested file should expand its ancestors before collapsing all");
  const SDL_FRect button_rect = WorkspaceShellTestAccess::TreeSidebarCollapseButtonRect(shell);
  Expect(SendMouseDown(
             shell, button_rect.x + button_rect.w * 0.5f, button_rect.y + button_rect.h * 0.5f,
             SDL_BUTTON_LEFT),
         "clicking the collapse button should be handled");
  Expect(!tree_contains_path(source),
         "clicking the collapse button should hide descendants under expanded directories");
  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == (root / "src").lexically_normal(),
         "collapsing all should keep selection on the nearest still-visible ancestor");
}

void TestWorkspaceShellTreeHeaderCompactsBeforeButtonsOverlap() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path nested_dir = root / "src" / "nested";
  const std::filesystem::path source = nested_dir / "main.cpp";
  std::filesystem::create_directories(nested_dir);
  WriteFile(source, "int main() {}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 520, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  const SDL_FRect mode_rect = WorkspaceShellTestAccess::SidebarModeButtonRect(shell);
  const SDL_FRect collapse_rect = WorkspaceShellTestAccess::TreeSidebarCollapseButtonRect(shell);
  const SDL_FRect refresh_rect = WorkspaceShellTestAccess::TreeSidebarRefreshButtonRect(shell);

  Expect(mode_rect.w > 0.0f && collapse_rect.w > 0.0f && refresh_rect.w > 0.0f,
         "compact tree-header fixture should still expose all header controls");
  Expect(mode_rect.x + mode_rect.w <= collapse_rect.x,
         "sidebar mode control should compact before overlapping the collapse button");
  Expect(collapse_rect.x + collapse_rect.w <= refresh_rect.x,
         "collapse and refresh controls should remain non-overlapping in compact header mode");
  Expect(SendMouseDown(
             shell, collapse_rect.x + collapse_rect.w * 0.5f, collapse_rect.y + collapse_rect.h * 0.5f,
             SDL_BUTTON_LEFT),
         "compact collapse button should remain clickable after header compaction");
}

void TestWorkspaceShellTabSizeSettingAppliesImmediately() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  Expect(WorkspaceShellTestAccess::EditorTabSize(shell) == 4,
         "tab-size settings fixture should start from the default tab size");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).tab_size() == 4,
         "active editor should start with the default tab size");
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.tab_size", "2"),
         "setting editor.tab_size should succeed through the settings path");
  Expect(WorkspaceShellTestAccess::EditorTabSize(shell) == 2,
         "editor tab-size preference should update immediately after setting change");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).tab_size() == 2,
         "active editor viewport should apply the new tab-size preference immediately");

}

void TestWorkspaceShellTabSizeSettingStaysVisibleAfterRestart() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config";
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.tab_size", "2"),
         "setting editor.tab_size should succeed before restart");

  WorkspaceShell reloaded_shell;
  WorkspaceShellTestAccess::SetProjectRoot(reloaded_shell, root);
  Expect(WorkspaceShellTestAccess::RestoreConfigState(reloaded_shell),
         "restarted shell should restore project config state");
  const auto stored_tab_size =
      WorkspaceShellTestAccess::ProjectStoredSettingValue(reloaded_shell, "editor.tab_size");
  Expect(stored_tab_size.has_value() && *stored_tab_size == "2",
         "restored project settings should retain editor.tab_size for settings overlay display");
}

void TestWorkspaceShellCommandTabSizeStaysVisibleAfterRestart() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config";
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(ExecuteCommand(shell, "tab-size 5"),
         "tab-size command should update editor preferences and persist project config");

  WorkspaceShell reloaded_shell;
  WorkspaceShellTestAccess::SetProjectRoot(reloaded_shell, root);
  Expect(WorkspaceShellTestAccess::RestoreConfigState(reloaded_shell),
         "restarted shell should restore project config state after tab-size command");
  const auto stored_tab_size =
      WorkspaceShellTestAccess::ProjectStoredSettingValue(reloaded_shell, "editor.tab_size");
  Expect(stored_tab_size.has_value() && *stored_tab_size == "5",
         "restored settings list should mirror canonical tab size after command-driven updates");
}

void TestWorkspaceShellSettingsOverlayRightClickDoesNotOpenEditorContextMenu() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);

  Expect(WorkspaceShellTestAccess::SettingsOverlayVisible(shell),
         "settings overlay fixture should open the overlay");
  const SDL_FRect overlay_rect = WorkspaceShellTestAccess::SettingsOverlayRect(shell);
  Expect(SendMouseDown(shell, overlay_rect.x + overlay_rect.w * 0.5f,
                       overlay_rect.y + overlay_rect.h * 0.5f, SDL_BUTTON_RIGHT),
         "right click inside settings overlay should be handled");
  Expect(!WorkspaceShellTestAccess::EditorContextMenuOpen(shell),
         "right click inside settings overlay should not leak into editor context menu");
}

void TestWorkspaceShellIgnoredTreeFileActivatesDirectOpenPath() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path ignored_file = root / ".env.local";
  WriteFile(root / ".gitignore", ".env.local\n");
  WriteFile(ignored_file, "TOKEN=abc\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  const auto& entries = WorkspaceShellTestAccess::TreeEntries(shell);
  const auto ignored_it = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
    return entry.path == ignored_file.lexically_normal();
  });
  Expect(ignored_it != entries.end(),
         "ignored file should remain visible in the project tree");
  Expect(ignored_it != entries.end() && ignored_it->ignored,
         "ignored file should be marked ignored in the tree model");

  Expect(WorkspaceShellTestAccess::SelectTreePath(shell, ignored_file),
         "ignored file should be selectable in the tree");
  const auto activated = WorkspaceShellTestAccess::ActivateTreeSelection(shell);
  Expect(activated.has_value() && activated->lexically_normal() == ignored_file.lexically_normal(),
         "activating an ignored file selection should return a direct-open file path");
}

void TestWorkspaceShellIgnoredDirectoryExpansionMaterializesOneLevel() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path ignored_dir = root / "node_modules";
  const std::filesystem::path nested_dir = ignored_dir / "pkg";
  const std::filesystem::path nested_file = nested_dir / "deep.js";
  const std::filesystem::path immediate_file = ignored_dir / "top.js";
  WriteFile(root / ".gitignore", "node_modules/\n");
  WriteFile(immediate_file, "console.log('top');\n");
  WriteFile(nested_file, "console.log('deep');\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  const auto contains_path = [&](const std::filesystem::path& path) {
    const auto& tree_entries = WorkspaceShellTestAccess::TreeEntries(shell);
    return std::any_of(tree_entries.begin(), tree_entries.end(),
                       [&](const auto& entry) { return entry.path == path.lexically_normal(); });
  };

  Expect(contains_path(ignored_dir),
         "ignored directory should be visible before expansion");
  Expect(!contains_path(immediate_file) && !contains_path(nested_file),
         "ignored descendants should remain unmaterialized before expansion");

  Expect(WorkspaceShellTestAccess::SelectTreePath(shell, ignored_dir),
         "ignored directory should be selectable");
  WorkspaceShellTestAccess::ExpandTreeSelection(shell);

  Expect(contains_path(immediate_file),
         "expanding ignored directory should materialize immediate children");
  Expect(contains_path(nested_dir),
         "expanding ignored directory should materialize immediate child directories");
  Expect(!contains_path(nested_file),
         "expanding ignored directory should not recursively materialize deeper descendants");
}

void TestWorkspaceShellHiddenIgnoredDirectoryUsesSameLazyExpansionRules() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path hidden_ignored_dir = root / ".cache";
  const std::filesystem::path nested_dir = hidden_ignored_dir / "pkg";
  const std::filesystem::path nested_file = nested_dir / "deep.bin";
  const std::filesystem::path immediate_file = hidden_ignored_dir / "top.bin";
  WriteFile(root / ".gitignore", ".cache/\n");
  WriteFile(immediate_file, "top\n");
  WriteFile(nested_file, "deep\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  const auto contains_path = [&](const std::filesystem::path& path) {
    const auto& tree_entries = WorkspaceShellTestAccess::TreeEntries(shell);
    return std::any_of(tree_entries.begin(), tree_entries.end(),
                       [&](const auto& entry) { return entry.path == path.lexically_normal(); });
  };

  Expect(contains_path(hidden_ignored_dir),
         "hidden ignored directories should be visible like other ignored directories");
  Expect(!contains_path(immediate_file) && !contains_path(nested_file),
         "hidden ignored descendants should remain unmaterialized before expansion");

  Expect(WorkspaceShellTestAccess::SelectTreePath(shell, hidden_ignored_dir),
         "hidden ignored directory should be selectable");
  WorkspaceShellTestAccess::ExpandTreeSelection(shell);

  Expect(contains_path(immediate_file),
         "expanding hidden ignored directories should materialize immediate children");
  Expect(!contains_path(nested_file),
         "expanding hidden ignored directories should still avoid recursive materialization");
}

void TestWorkspaceShellCopySelectionWithContextUsesRelativePathAndLineRange() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source_dir = root / "src";
  const std::filesystem::path source = source_dir / "main.cpp";
  std::filesystem::create_directories(source_dir);
  WriteFile(source, "int main() {\n  int value = 1;\n  return value;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 2);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(2, 15, true);

  Expect(WorkspaceShellTestAccess::ExecuteCopySelectionWithContext(shell),
         "copy with context action should execute");
  Expect(clipboard_text == "src/main.cpp:2-3\nint value = 1;\n  return value;",
         "copy with context should prepend the relative path and selected line range");
}

void TestWorkspaceShellEditorRightClickOpensSymbolAwareContextMenu() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  std::filesystem::create_directories(root);
  WriteFile(source, "int alpha = beta;\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float click_x =
      metrics.text_x + WorkspaceShellTestAccess::TextCharWidth(shell) * 12.0f;
  const float click_y = metrics.first_line_y + metrics.line_height * 0.5f;

  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.button = SDL_BUTTON_RIGHT;
  event.button.x = click_x;
  event.button.y = click_y;

  Expect(shell.HandleEvent(event).handled,
         "right-clicking the editor should be handled");
  Expect(WorkspaceShellTestAccess::MenuBarOpen(shell),
         "right-clicking the editor should open a popup menu");
  Expect(WorkspaceShellTestAccess::EditorContextMenuOpen(shell),
         "right-clicking the editor should open the dedicated editor context menu");
  Expect(!WorkspaceShellTestAccess::EditMenuOpen(shell),
         "right-clicking the editor should not light up the top-level Edit menu");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 0 &&
             WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == 12,
         "right-clicking a symbol should retarget the caret before opening the context menu");
}

void OpenSplitEditorMouseFixture(WorkspaceShell& shell,
                                 TemporaryDirectory& temp_dir,
                                 std::filesystem::path* left_path,
                                 std::filesystem::path* right_path) {
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path left = root / "left.txt";
  const std::filesystem::path right = root / "right.txt";
  WriteFile(left, "left line 1\nleft line 2\nleft line 3\n");
  WriteFile(right, "right line 1\nright line 2\nright line 3\n");

  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, left);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::SplitActiveEditor(shell),
         "editor mouse fixture should open a split pane");
  Expect(WorkspaceShellTestAccess::ReplaceActiveEditorWithFile(shell, right),
         "editor mouse fixture should load the second file into the active pane");

  if (left_path != nullptr) {
    *left_path = left;
  }
  if (right_path != nullptr) {
    *right_path = right;
  }
}

void TestWorkspaceShellClickingInactiveEditorPaneActivatesSplit() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  std::filesystem::path left;
  std::filesystem::path right;
  OpenSplitEditorMouseFixture(shell, temp_dir, &left, &right);

  Expect(WorkspaceShellTestAccess::ActivateOrderedEditorSplit(shell, 1),
         "split click fixture should expose the right pane");
  const SDL_FRect right_rect = WorkspaceShellTestAccess::ActiveEditorPaneRect(shell);
  Expect(WorkspaceShellTestAccess::ActivateOrderedEditorSplit(shell, 0),
         "split click fixture should restore the left pane before the click");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == left.lexically_normal(),
         "split click fixture should start from the left editor");

  const float click_x = right_rect.x + right_rect.w * 0.5f;
  const float click_y = right_rect.y + right_rect.h * 0.5f;
  Expect(SendMouseDown(shell, click_x, click_y, SDL_BUTTON_LEFT),
         "clicking an inactive editor pane should be handled");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == right.lexically_normal(),
         "clicking an inactive editor pane should activate that split");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell),
         "clicking an inactive editor pane should keep editor focus");
}

void TestWorkspaceShellEditorWheelActivatesHoveredSplit() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  std::filesystem::path left;
  std::filesystem::path right;
  OpenSplitEditorMouseFixture(shell, temp_dir, &left, &right);

  Expect(WorkspaceShellTestAccess::ActivateOrderedEditorSplit(shell, 1),
         "split wheel fixture should expose the right pane");
  const SDL_FRect right_rect = WorkspaceShellTestAccess::ActiveEditorPaneRect(shell);
  Expect(WorkspaceShellTestAccess::ActivateOrderedEditorSplit(shell, 0),
         "split wheel fixture should restore the left pane before scrolling");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == left.lexically_normal(),
         "split wheel fixture should start from the left editor");

  const float wheel_x = right_rect.x + right_rect.w * 0.5f;
  const float wheel_y = right_rect.y + right_rect.h * 0.5f;
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, -1),
         "scrolling over an inactive editor pane should be handled");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == right.lexically_normal(),
         "scrolling over an inactive editor pane should activate that split first");
}

void TestWorkspaceShellEditorDragSelectionTracksPointer() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.txt";
  WriteFile(source, "alpha beta\nsecond line\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float char_width = WorkspaceShellTestAccess::TextCharWidth(shell);
  const float y = metrics.first_line_y + metrics.line_height * 0.5f;
  const float start_x = metrics.text_x + char_width * 0.1f;
  const float end_x = metrics.text_x + char_width * 5.1f;

  Expect(SendMouseDown(shell, start_x, y, SDL_BUTTON_LEFT),
         "pressing inside the editor should start mouse selection");
  Expect(SendMouseMotion(shell, end_x, y, SDL_BUTTON_LMASK),
         "dragging inside the editor should update mouse selection");
  Expect(SendMouseUp(shell, end_x, y, SDL_BUTTON_LEFT),
         "releasing after an editor drag should be handled");
  Expect(WorkspaceShellTestAccess::ActiveEditorHasSelection(shell),
         "dragging across editor text should create a selection");
  Expect(WorkspaceShellTestAccess::ActiveEditorSelectedText(shell) == "alpha",
         "editor drag selection should capture the dragged text range");
}

void TestWorkspaceShellAltClickAddsSecondaryCaret() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.txt";
  WriteFile(source, "alpha beta\nsecond line\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.MoveCursorTo(0, 0);

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float char_width = WorkspaceShellTestAccess::TextCharWidth(shell);
  const float y = metrics.first_line_y + metrics.line_height * 0.5f;
  const float click_x = metrics.text_x + char_width * 5.0f;

  const SDL_Keymod previous_mods = SDL_GetModState();
  SDL_SetModState(static_cast<SDL_Keymod>(previous_mods | SDL_KMOD_ALT));
  const bool handled = SendMouseDown(shell, click_x, y, SDL_BUTTON_LEFT);
  SDL_SetModState(previous_mods);

  Expect(handled, "Alt+left click inside the editor should be handled");
  Expect(viewport.has_multiple_carets(),
         "Alt+left click should add a secondary caret");
  Expect(!viewport.secondary_carets().empty() &&
             viewport.secondary_carets().front() == microide::editor::TextPosition{0, 0},
         "Alt+left click should preserve the previous primary caret as secondary");
  Expect(viewport.cursor_column() == 5,
         "Alt+left click should move the primary caret to the clicked column");
}

void TestWorkspaceShellHoveredTabShowsRelativePathTooltip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "deep" / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect tab_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  SendMouseMotion(shell, tab_rect.x + tab_rect.w * 0.5f,
                                              tab_rect.y + tab_rect.h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredTabTooltipLabel(shell) == "src/deep/main.cpp",
         "hovering a tab should expose the full relative path tooltip");
}

void TestWorkspaceShellWindowMouseLeaveClearsTabTooltip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "deep" / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect tab_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  SendMouseMotion(shell, tab_rect.x + tab_rect.w * 0.5f,
                                              tab_rect.y + tab_rect.h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredTabTooltipLabel(shell) == "src/deep/main.cpp",
         "tab tooltip fixture should start with a hovered tab label");

  Expect(SendWindowMouseLeave(shell),
         "window mouse leave should be handled");
  Expect(WorkspaceShellTestAccess::HoveredTabTooltipLabel(shell).empty(),
         "window mouse leave should clear stale tab tooltip hover state");
}

void TestWorkspaceShellInWindowMouseMoveClearsProjectTabTooltipAndInvalidatesChrome() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path first_root = temp_dir.path() / "first-project";
  const std::filesystem::path second_root = temp_dir.path() / "second-project";
  const std::filesystem::path second_file = second_root / "src" / "deep" / "main.cpp";
  WriteFile(first_root / "README.md", "first\n");
  WriteFile(second_file, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, first_root, false, false),
         "first project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, second_root, false, false),
         "second project should open");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect project_tab_rect = WorkspaceShellTestAccess::ProjectTabRect(shell, 1);
  SendMouseMotion(shell, project_tab_rect.x + project_tab_rect.w * 0.5f,
                  project_tab_rect.y + project_tab_rect.h * 0.5f, 0);
  const std::string hovered_label_before =
      WorkspaceShellTestAccess::HoveredProjectTabTooltipLabel(shell);
  Expect(!hovered_label_before.empty(),
         "project tab tooltip fixture should start with a hovered project tab label");
  (void)shell.ConsumePendingRenderInvalidation();

  const SDL_FRect editor_rect = WorkspaceShellTestAccess::ActiveEditorPaneRect(shell);
  SendMouseMotion(shell, editor_rect.x + 20.0f, editor_rect.y + 20.0f, 0);
  Expect(WorkspaceShellTestAccess::HoveredProjectTabTooltipLabel(shell).empty(),
         "moving inside the window away from project tabs should clear project tab tooltip hover");
}

void TestWorkspaceShellEditorSelectionWritesPrimaryBufferAndMiddleClickPastes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "main.txt";
  WriteFile(file, "hello world\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, file);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  std::string primary_selection;
  WorkspaceShellTestAccess::SetPrimarySelectionTextWriter(
      shell, [&](std::string_view text) {
        primary_selection = std::string(text);
        return true;
      });
  WorkspaceShellTestAccess::SetPrimarySelectionTextReader(
      shell, [&]() -> std::optional<std::string> { return primary_selection; });

  const SDL_FRect pane = WorkspaceShellTestAccess::ActiveEditorPaneRect(shell);
  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  const float start_x = metrics.text_x + 1.0f;
  const float end_x = metrics.text_x + WorkspaceShellTestAccess::TextCharWidth(shell) * 5.0f + 1.0f;
  const float y = metrics.first_line_y + std::min(4.0f, pane.h * 0.25f);

  Expect(SendMouseDown(shell, start_x, y, SDL_BUTTON_LEFT),
         "starting an editor drag selection should be handled");
  Expect(SendMouseMotion(shell, end_x, y, SDL_BUTTON_LMASK),
         "dragging an editor selection should be handled");
  Expect(SendMouseUp(shell, end_x, y, SDL_BUTTON_LEFT),
         "releasing an editor selection should be handled");
  Expect(primary_selection == "hello",
         "editor drag selection should update the primary selection buffer");

  const float paste_x = metrics.text_x + WorkspaceShellTestAccess::TextCharWidth(shell) * 11.0f + 1.0f;
  Expect(SendMouseDown(shell, paste_x, y, SDL_BUTTON_MIDDLE),
         "middle-clicking the editor should be handled");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines()[0] == "hello worldhello",
         "middle-clicking the editor should paste the primary selection at the click location");
}

void TestWorkspaceShellTextInputSurfaceTracksEditorOverlayAndPrompt() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "main.txt";
  WriteFile(file, "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, file);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(WorkspaceShellTestAccess::TextInputSurfaceIsEditor(shell),
         "the active editor should own text input by default");

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "opening the file-finder overlay should be handled");
  Expect(WorkspaceShellTestAccess::TextInputSurfaceIsFileFinder(shell),
         "the file-finder overlay should take text-input ownership when visible");

  WorkspaceShellTestAccess::PrepareRenamePrompt(shell, file, "renamed.txt");
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "rename prompt fixture should open the prompt surface");
  Expect(WorkspaceShellTestAccess::TextInputSurfaceIsPromptInput(shell),
         "prompt input should override the underlying overlay or editor text-input surface");
}

void TestWorkspaceShellSidebarModeButtonTogglesAnchoredMenu() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect button_rect = WorkspaceShellTestAccess::SidebarModeButtonRect(shell);
  const float click_x = button_rect.x + button_rect.w * 0.5f;
  const float click_y = button_rect.y + button_rect.h * 0.5f;

  Expect(SendMouseDown(shell, click_x, click_y, SDL_BUTTON_LEFT),
         "clicking the sidebar mode control should be handled");
  Expect(WorkspaceShellTestAccess::SidebarModeMenuOpen(shell),
         "clicking the sidebar mode control should open its anchored menu");
  Expect(WorkspaceShellTestAccess::FocusIsSidebar(shell),
         "opening the sidebar mode menu should keep sidebar focus");

  Expect(SendMouseDown(shell, click_x, click_y, SDL_BUTTON_LEFT),
         "clicking the active sidebar mode control again should be handled");
  Expect(!WorkspaceShellTestAccess::MenuBarOpen(shell),
         "clicking the active sidebar mode control again should close the anchored menu");
}

void TestWorkspaceShellProjectTabsDragReorderToEnd() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  const std::filesystem::path root_c = temp_dir.path() / "gamma-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");
  WriteFile(root_c / "README.md", "gamma\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_c, false, false),
         "third project should open");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect source_rect = WorkspaceShellTestAccess::ProjectTabRect(shell, 0);
  Expect(SendMouseDown(
             shell, source_rect.x + source_rect.w * 0.5f, source_rect.y + source_rect.h * 0.5f,
             SDL_BUTTON_LEFT),
         "dragging should start from a project tab press");

  const SDL_FRect last_rect = WorkspaceShellTestAccess::ProjectTabRect(shell, 2);
  const float drop_x = last_rect.x + last_rect.w + 12.0f;
  const float drop_y = last_rect.y + last_rect.h * 0.5f;
  Expect(SendMouseMotion(shell, drop_x, drop_y, SDL_BUTTON_LMASK),
         "dragging across the project tab strip should be handled");
  Expect(SendMouseUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT),
         "releasing a dragged project tab should be handled");

  Expect(WorkspaceShellTestAccess::ProjectRoots(shell) ==
             std::vector<std::filesystem::path>{root_b.lexically_normal(),
                                                root_c.lexically_normal(),
                                                root_a.lexically_normal()},
         "dragging a project tab to the end should reorder the project strip");
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(shell) == 2,
         "dragged project tab should stay active after reordering");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_a.lexically_normal(),
         "dragged project should remain the active workspace");
}

void TestWorkspaceShellProjectOpenExistingRootSwitchesWithoutDuplicatingCatalog() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "reopening an existing project root should succeed");

  Expect(WorkspaceShellTestAccess::ProjectCount(shell) == 2,
         "reopening an existing project should not duplicate the project catalog");
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(shell) == 0,
         "reopening an existing project should activate its existing project tab");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_a.lexically_normal(),
         "reopening an existing project should restore that project as active");
}

void TestWorkspaceShellProjectOpenFailureRestoresPreviousActiveProject() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path readme = root / "README.md";
  WriteFile(readme, "alpha\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "baseline project should open");
  Expect(!WorkspaceShellTestAccess::OpenProjectTab(shell, readme, false, false),
         "opening a file path as a project should fail");

  Expect(WorkspaceShellTestAccess::ProjectCount(shell) == 1,
         "failed project opens should roll back their catalog insertion");
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(shell) == 0,
         "failed project opens should preserve the previous active project index");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root.lexically_normal(),
         "failed project opens should restore the previous active workspace");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1 &&
             WorkspaceShellTestAccess::OpenTabs(shell).front().path == readme.lexically_normal(),
         "failed project opens should keep the previous project's tab state intact");
}

void TestWorkspaceShellCloseActiveProjectRestoresAdjacentProject() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  const std::filesystem::path root_c = temp_dir.path() / "gamma-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");
  WriteFile(root_c / "README.md", "gamma\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "first project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "second project should open");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_c, false, false),
         "third project should open");
  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 1, false),
         "middle project should become active before the close test");

  WorkspaceShellTestAccess::CloseProject(shell, 1);

  Expect(WorkspaceShellTestAccess::ProjectRoots(shell) ==
             std::vector<std::filesystem::path>{root_a.lexically_normal(),
                                                root_c.lexically_normal()},
         "closing the active project should remove only that project from the catalog");
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(shell) == 1,
         "closing the active middle project should restore the adjacent project at that slot");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_c.lexically_normal(),
         "closing the active middle project should activate the adjacent surviving workspace");
}

void TestWorkspaceShellEditorTabsDragReorderBetweenTabs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file_a = root / "alpha.cpp";
  const std::filesystem::path file_b = root / "beta.cpp";
  const std::filesystem::path file_c = root / "gamma.cpp";
  WriteFile(file_a, "int alpha() { return 1; }\n");
  WriteFile(file_b, "int beta() { return 2; }\n");
  WriteFile(file_c, "int gamma() { return 3; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_a),
         "first editor tab should open");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_b),
         "second editor tab should open");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file_c),
         "third editor tab should open");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect source_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  Expect(SendMouseDown(
             shell, source_rect.x + source_rect.w * 0.5f, source_rect.y + source_rect.h * 0.5f,
             SDL_BUTTON_LEFT),
         "dragging should start from an editor tab press");

  const SDL_FRect third_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 2);
  const float drop_x = third_rect.x + 1.0f;
  const float drop_y = third_rect.y + third_rect.h * 0.5f;
  Expect(SendMouseMotion(shell, drop_x, drop_y, SDL_BUTTON_LMASK),
         "dragging across editor tabs should be handled");
  Expect(SendMouseUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT),
         "releasing a dragged editor tab should be handled");

  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(shell);
  Expect(tabs.size() == 3, "editor tab reorder should keep the same tab count");
  Expect(tabs[0].path == file_b.lexically_normal() && tabs[1].path == file_a.lexically_normal() &&
             tabs[2].path == file_c.lexically_normal(),
         "dragging an editor tab between tabs should reorder it into the requested slot");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 1,
         "dragged editor tab should stay active after reordering");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == file_a.lexically_normal(),
         "dragged editor tab should keep its buffer active");
}

void TestWorkspaceShellProjectTabWheelScrollsStrip() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;

  for (int i = 0; i < 8; ++i) {
    const std::filesystem::path root =
        temp_dir.path() / ("project-" + std::to_string(i) + "-with-a-long-name");
    WriteFile(root / "README.md", "root\n");
    Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
           "project fixture should open");
  }
  WorkspaceShellTestAccess::SetWindowSize(shell, 640, 720);
  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "first project should become active before the wheel scroll test");
  const SDL_FRect first_rect = WorkspaceShellTestAccess::ProjectTabRect(shell, 0);

  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_WHEEL;
  event.wheel.integer_y = -2;
  event.wheel.y = -2.0f;
  event.wheel.mouse_x = first_rect.x + first_rect.w * 0.5f;
  event.wheel.mouse_y = first_rect.y + first_rect.h * 0.5f;

  Expect(shell.HandleEvent(event).handled, "mouse wheel over the project strip should be handled");
  Expect(WorkspaceShellTestAccess::ProjectTabScrollIndex(shell) == 2,
         "mouse wheel over the project strip should update the project strip scroll index");
}

void TestWorkspaceShellEditorTabWheelScrollsStrip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  for (int i = 0; i < 10; ++i) {
    const std::filesystem::path file =
        root / ("file-" + std::to_string(i) + "-with-a-very-long-name.cpp");
    WriteFile(file, "int value() { return 1; }\n");
    Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, file),
           "editor tab fixture should open");
  }
  WorkspaceShellTestAccess::SetWindowSize(shell, 640, 720);
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  const SDL_FRect first_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);

  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_WHEEL;
  event.wheel.integer_y = -3;
  event.wheel.y = -3.0f;
  event.wheel.mouse_x = first_rect.x + first_rect.w * 0.5f;
  event.wheel.mouse_y = first_rect.y + first_rect.h * 0.5f;

  Expect(shell.HandleEvent(event).handled, "mouse wheel over the editor strip should be handled");
  Expect(WorkspaceShellTestAccess::EditorTabScrollIndex(shell) == 3,
         "mouse wheel over the editor strip should update the editor strip scroll index");
}

void TestWorkspaceShellProjectWatcherReloadDoesNotContinuouslyRearm() {
  static const bool initialized = SDL_Init(SDL_INIT_VIDEO);
  Expect(initialized, "SDL video initialization should succeed for watcher event tests");

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project watcher fixture should open the project");
  const auto idle_delay_before = shell.NextAnimationDelayMs();
  Expect(!idle_delay_before.has_value() || *idle_delay_before > 0,
         "idle project watchers should not schedule a zero-delay wake when no change is pending");

  WriteFile(root / "watched.txt", "changed\n");
  Expect(WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, true),
         "project watcher reload should detect filesystem changes");
  bool settled = false;
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, true)) {
      settled = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Expect(settled,
         "project watcher reload should settle quickly instead of continuously rearming");
  const auto idle_delay_after = shell.NextAnimationDelayMs();
  Expect(!idle_delay_after.has_value() || *idle_delay_after > 0,
         "project watcher reload should settle without a zero-delay wake after refresh");
}

void TestWorkspaceShellProjectWatcherIgnoresGitignoredDirectories() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / ".gitignore", "node_modules/\n");
  WriteFile(root / "README.md", "root\n");
  WriteFile(root / "node_modules" / "pkg" / "index.js", "module.exports = 1;\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "ignored-directory watcher fixture should open the project");

  WriteFile(root / "node_modules" / "pkg" / "index.js", "module.exports = 2;\n");
  Expect(!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, true),
         "project watcher should ignore gitignored directory changes");

  WriteFile(root / "watched.txt", "changed\n");
  Expect(WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, true),
         "project watcher should still detect visible project changes");
}

void TestWorkspaceShellProjectWatcherIgnoresGitMetadataLockfiles() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / ".git" / "HEAD", "ref: refs/heads/main\n");
  WriteFile(root / "README.md", "root\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::RegisterLifecycleWakeEvents(shell);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "git-metadata watcher fixture should open the project");
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  WriteFile(root / ".git" / "index.lock", "lock\n");
  Expect(!WaitForProjectReload(shell, std::chrono::milliseconds(400)),
         "project watcher should ignore .git lockfile churn");
  Expect(!WaitForFileIndexPath(shell, std::filesystem::path(".git/index.lock"), true,
                               std::chrono::milliseconds(50)),
         "file index should not include .git lockfiles");

  WriteFile(root / "watched.txt", "changed\n");
  Expect(WaitForProjectReload(shell, std::chrono::milliseconds(1000)),
         "project watcher should still detect visible project changes after ignoring git locks");
}

void TestWorkspaceShellFileIndexUpdatesDoNotReloadCleanBuffers() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "index-only reload fixture should open the project");
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (!WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  WorkspaceShellTestAccess::ResetReloadCleanOpenBuffersFromDiskInvocationCount(shell);
  WorkspaceShellTestAccess::SetFileIndexHasPendingChanges(shell, true);

  Expect(WorkspaceShellTestAccess::ReloadProjectIfFilesChanged(shell, false),
         "index-only watcher updates should still refresh project state");
  Expect(WorkspaceShellTestAccess::ReloadCleanOpenBuffersFromDiskInvocationCount(shell) == 0,
         "file index updates alone should not reopen clean editor buffers from disk");
}

void TestWorkspaceShellFileFinderReflectsFileIndexUpdates() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path readme = root / "README.md";
  const std::filesystem::path added_file = root / "new-indexed.cpp";
  WriteFile(readme, "root\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "file-index update fixture should open the project");

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "file finder should open on demand");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "new-indexed"),
         "typing a missing file query should be handled");
  Expect(WorkspaceShellTestAccess::FileFinderResultCount(shell) == 0,
         "file finder should have no result before the file exists");
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "Escape should close the overlay after the missing file query");

  WriteFile(added_file, "int value() { return 7; }\n");
  Expect(WaitForProjectReload(shell, std::chrono::milliseconds(1000)),
         "project reload should observe a newly added file via the file index watcher");
  Expect(WaitForFileIndexPath(shell, std::filesystem::path("new-indexed.cpp"), true,
                              std::chrono::milliseconds(1000)),
         "file index should include the newly added file");

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "file finder should open after a file-index update");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "new-indexed"),
         "typing the added file query should be handled");
  Expect(WorkspaceShellTestAccess::FileFinderResultCount(shell) >= 1,
         "file finder should include the newly indexed file");
  const auto selected_path = WorkspaceShellTestAccess::FileFinderSelectedPath(shell);
  Expect(selected_path.has_value() &&
             selected_path->lexically_normal() ==
                 std::filesystem::path("new-indexed.cpp"),
         "file finder should select the newly indexed file path");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "pressing Enter should open the newly indexed file");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == added_file.lexically_normal(),
         "opening from file finder should land on the newly indexed file");

  std::error_code remove_error;
  std::filesystem::remove(added_file, remove_error);
  Expect(!remove_error, "fixture cleanup should remove the indexed file");
  Expect(WaitForProjectReload(shell, std::chrono::milliseconds(1000)),
         "project reload should observe file deletion via the file index watcher");
  Expect(WaitForFileIndexPath(shell, std::filesystem::path("new-indexed.cpp"), false,
                              std::chrono::milliseconds(1000)),
         "file index should remove the deleted file");

  Expect(WorkspaceShellTestAccess::ExecuteFilesFromShortcut(shell),
         "file finder should still open after file deletion");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "new-indexed"),
         "typing the deleted file query should be handled");
  Expect(WorkspaceShellTestAccess::FileFinderResultCount(shell) == 0,
         "file finder should no longer list the deleted file");
}

}  // namespace

void RegisterWorkspaceShellProjectTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/ProjectOpenMenuUsesNativePickerSelection",
          TestWorkspaceShellProjectOpenMenuUsesNativePickerSelection);
  AddTest(tests, "WorkspaceShell/ProjectOpenCommandUsesNativePickerAtActiveProjectRoot",
          TestWorkspaceShellProjectOpenCommandUsesNativePickerAtActiveProjectRoot);
  AddTest(tests, "WorkspaceShell/ProjectOpenMenuFallsBackToTypedPathWhenNativePickerFails",
          TestWorkspaceShellProjectOpenMenuFallsBackToTypedPathWhenNativePickerFails);
  AddTest(tests, "WorkspaceShell/ProjectOpenDefersGitSidebarRefreshUntilShown",
          TestWorkspaceShellProjectOpenDefersGitSidebarRefreshUntilShown);
  AddTest(tests, "WorkspaceShell/GitSidebarRefreshDispatchIsNonBlocking",
          TestWorkspaceShellGitSidebarRefreshDispatchIsNonBlocking);
  AddTest(tests, "WorkspaceShell/ProjectSwitchDiscardsStaleGitSidebarRefreshResult",
          TestWorkspaceShellProjectSwitchDiscardsStaleGitSidebarRefreshResult);
  AddTest(tests, "WorkspaceShell/ProjectWatcherIgnoresGitignoredDirectories",
          TestWorkspaceShellProjectWatcherIgnoresGitignoredDirectories);
  AddTest(tests, "WorkspaceShell/ProjectWatcherIgnoresGitMetadataLockfiles",
          TestWorkspaceShellProjectWatcherIgnoresGitMetadataLockfiles);
  AddTest(tests, "WorkspaceShell/FileIndexUpdatesDoNotReloadCleanBuffers",
          TestWorkspaceShellFileIndexUpdatesDoNotReloadCleanBuffers);
  AddTest(tests, "WorkspaceShell/FileFinderReflectsFileIndexUpdates",
          TestWorkspaceShellFileFinderReflectsFileIndexUpdates);
  AddTest(tests, "WorkspaceShell/UnknownCommandKeepsPromptOpenWithFeedback",
          TestWorkspaceShellUnknownCommandKeepsPromptOpenWithFeedback);
  AddTest(tests, "WorkspaceShell/LeftCtrlShortcutOpensCommandPrompt",
          TestWorkspaceShellLeftCtrlShortcutOpensCommandPrompt);
  AddTest(tests, "WorkspaceShell/CommandReportsMissingProjectInsteadOfSilentNoOp",
          TestWorkspaceShellCommandReportsMissingProjectInsteadOfSilentNoOp);
  AddTest(tests, "WorkspaceShell/OpenCommandRequiresPath",
          TestWorkspaceShellOpenCommandRequiresPath);
  AddTest(tests, "WorkspaceShell/ProjectNextAndPrevCommandsCycleProjects",
          TestWorkspaceShellProjectNextAndPrevCommandsCycleProjects);
  AddTest(tests, "WorkspaceShell/ProjectSwitchPreservesProjectScopedCommandState",
          TestWorkspaceShellProjectSwitchPreservesProjectScopedCommandState);
  AddTest(tests, "WorkspaceShell/ProjectSwitchPreservesSearchSidebarSurfaceState",
          TestWorkspaceShellProjectSwitchPreservesSearchSidebarSurfaceState);
  AddTest(tests, "WorkspaceShell/ProjectSwitchClearsTransientInteractionState",
          TestWorkspaceShellProjectSwitchClearsTransientInteractionState);
  AddTest(tests, "WorkspaceShell/ProjectOpenShowsDefaultTerminalPanel",
          TestWorkspaceShellProjectOpenShowsDefaultTerminalPanel);
  AddTest(tests, "WorkspaceShell/TermCommandRequestsBottomPanelRedraw",
          TestWorkspaceShellTermCommandRequestsBottomPanelRedraw);
  AddTest(tests, "WorkspaceShell/ProjectOpenDefersProjectWatcherArming",
          TestWorkspaceShellProjectOpenDefersProjectWatcherArming);
  AddTest(tests, "WorkspaceShell/SidebarWidthCommandParsesTypedRequests",
          TestWorkspaceShellSidebarWidthCommandParsesTypedRequests);
  AddTest(tests, "WorkspaceShell/MergeCommandResolvesRelativePaths",
          TestWorkspaceShellMergeCommandResolvesRelativePaths);
  AddTest(tests, "WorkspaceShell/TabMoveCommandSupportsRelativeOffsets",
          TestWorkspaceShellTabMoveCommandSupportsRelativeOffsets);
  AddTest(tests, "WorkspaceShell/GotoAndJumpCommandsUseTypedNavigationRequests",
          TestWorkspaceShellGotoAndJumpCommandsUseTypedNavigationRequests);
  AddTest(tests, "WorkspaceShell/GotoTargetsActiveSplitViewport",
          TestWorkspaceShellGotoTargetsActiveSplitViewport);
  AddTest(tests, "WorkspaceShell/GlobalCommandsApplyTypedRequests",
          TestWorkspaceShellGlobalCommandsApplyTypedRequests);
  AddTest(tests, "WorkspaceShell/CommandPromptCompletionAndHistory",
          TestWorkspaceShellCommandPromptCompletionAndHistory);
  AddTest(tests, "WorkspaceShell/CtrlNOpensUntitledTab",
          TestWorkspaceShellCtrlNOpensUntitledTab);
  AddTest(tests, "WorkspaceShell/FilesShortcutEscapeRestoresSidebarFocus",
          TestWorkspaceShellFilesShortcutEscapeRestoresSidebarFocus);
  AddTest(tests, "WorkspaceShell/FilesShortcutEscapeRestoresEditorFocusOnWelcome",
          TestWorkspaceShellFilesShortcutEscapeRestoresEditorFocusOnWelcome);
  AddTest(tests, "WorkspaceShell/FilesShortcutOpensMatchedFileAfterDeferredIndexCacheBuild",
          TestWorkspaceShellFilesShortcutOpensMatchedFileAfterDeferredIndexCacheBuild);
  AddTest(tests, "WorkspaceShell/ProjectOpenFromWelcomeInvalidatesCachedLayout",
          TestWorkspaceShellProjectOpenFromWelcomeInvalidatesCachedLayout);
  AddTest(tests, "WorkspaceShell/OverlayOutsideClickRestoresPrimaryFocus",
          TestWorkspaceShellOverlayOutsideClickRestoresPrimaryFocus);
  AddTest(tests, "WorkspaceShell/TreeCollapseAllowsOpenDescendantsAndReselectReveal",
          TestWorkspaceShellTreeCollapseAllowsOpenDescendantsAndReselectReveal);
  AddTest(tests, "WorkspaceShell/TreeScrollDoesNotSnapToSelectionDuringRender",
          TestWorkspaceShellTreeScrollDoesNotSnapToSelectionDuringRender);
  AddTest(tests, "WorkspaceShell/TreeCollapseButtonCollapsesAllOpenDirectories",
          TestWorkspaceShellTreeCollapseButtonCollapsesAllOpenDirectories);
  AddTest(tests, "WorkspaceShell/TreeHeaderCompactsBeforeButtonsOverlap",
          TestWorkspaceShellTreeHeaderCompactsBeforeButtonsOverlap);
  AddTest(tests, "WorkspaceShell/TabSizeSettingAppliesImmediately",
          TestWorkspaceShellTabSizeSettingAppliesImmediately);
  AddTest(tests, "WorkspaceShell/TabSizeSettingStaysVisibleAfterRestart",
          TestWorkspaceShellTabSizeSettingStaysVisibleAfterRestart);
  AddTest(tests, "WorkspaceShell/CommandTabSizeStaysVisibleAfterRestart",
          TestWorkspaceShellCommandTabSizeStaysVisibleAfterRestart);
  AddTest(tests, "WorkspaceShell/SettingsOverlayRightClickDoesNotOpenEditorContextMenu",
          TestWorkspaceShellSettingsOverlayRightClickDoesNotOpenEditorContextMenu);
  AddTest(tests, "WorkspaceShell/IgnoredTreeFileActivatesDirectOpenPath",
          TestWorkspaceShellIgnoredTreeFileActivatesDirectOpenPath);
  AddTest(tests, "WorkspaceShell/IgnoredDirectoryExpansionMaterializesOneLevel",
          TestWorkspaceShellIgnoredDirectoryExpansionMaterializesOneLevel);
  AddTest(tests, "WorkspaceShell/HiddenIgnoredDirectoryUsesSameLazyExpansionRules",
          TestWorkspaceShellHiddenIgnoredDirectoryUsesSameLazyExpansionRules);
  AddTest(tests, "WorkspaceShell/CopySelectionWithContextUsesRelativePathAndLineRange",
          TestWorkspaceShellCopySelectionWithContextUsesRelativePathAndLineRange);
  AddTest(tests, "WorkspaceShell/EditorRightClickOpensSymbolAwareContextMenu",
          TestWorkspaceShellEditorRightClickOpensSymbolAwareContextMenu);
  AddTest(tests, "WorkspaceShell/ClickingInactiveEditorPaneActivatesSplit",
          TestWorkspaceShellClickingInactiveEditorPaneActivatesSplit);
  AddTest(tests, "WorkspaceShell/EditorWheelActivatesHoveredSplit",
          TestWorkspaceShellEditorWheelActivatesHoveredSplit);
  AddTest(tests, "WorkspaceShell/EditorDragSelectionTracksPointer",
          TestWorkspaceShellEditorDragSelectionTracksPointer);
  AddTest(tests, "WorkspaceShell/AltClickAddsSecondaryCaret",
          TestWorkspaceShellAltClickAddsSecondaryCaret);
  AddTest(tests, "WorkspaceShell/HoveredTabShowsRelativePathTooltip",
          TestWorkspaceShellHoveredTabShowsRelativePathTooltip);
  AddTest(tests, "WorkspaceShell/WindowMouseLeaveClearsTabTooltip",
          TestWorkspaceShellWindowMouseLeaveClearsTabTooltip);
  AddTest(tests, "WorkspaceShell/InWindowMouseMoveClearsProjectTabTooltipAndInvalidatesChrome",
          TestWorkspaceShellInWindowMouseMoveClearsProjectTabTooltipAndInvalidatesChrome);
  AddTest(tests, "WorkspaceShell/EditorSelectionWritesPrimaryBufferAndMiddleClickPastes",
          TestWorkspaceShellEditorSelectionWritesPrimaryBufferAndMiddleClickPastes);
  AddTest(tests, "WorkspaceShell/TextInputSurfaceTracksEditorOverlayAndPrompt",
          TestWorkspaceShellTextInputSurfaceTracksEditorOverlayAndPrompt);
  AddTest(tests, "WorkspaceShell/SidebarModeButtonTogglesAnchoredMenu",
          TestWorkspaceShellSidebarModeButtonTogglesAnchoredMenu);
  AddTest(tests, "WorkspaceShell/ProjectOpenExistingRootSwitchesWithoutDuplicatingCatalog",
          TestWorkspaceShellProjectOpenExistingRootSwitchesWithoutDuplicatingCatalog);
  AddTest(tests, "WorkspaceShell/ProjectOpenFailureRestoresPreviousActiveProject",
          TestWorkspaceShellProjectOpenFailureRestoresPreviousActiveProject);
  AddTest(tests, "WorkspaceShell/CloseActiveProjectRestoresAdjacentProject",
          TestWorkspaceShellCloseActiveProjectRestoresAdjacentProject);
  AddTest(tests, "WorkspaceShell/ProjectTabsDragReorderToEnd",
          TestWorkspaceShellProjectTabsDragReorderToEnd);
  AddTest(tests, "WorkspaceShell/EditorTabsDragReorderBetweenTabs",
          TestWorkspaceShellEditorTabsDragReorderBetweenTabs);
  AddTest(tests, "WorkspaceShell/ProjectTabWheelScrollsStrip",
          TestWorkspaceShellProjectTabWheelScrollsStrip);
  AddTest(tests, "WorkspaceShell/EditorTabWheelScrollsStrip",
          TestWorkspaceShellEditorTabWheelScrollsStrip);
  AddTest(tests, "WorkspaceShell/ProjectWatcherReloadDoesNotContinuouslyRearm",
          TestWorkspaceShellProjectWatcherReloadDoesNotContinuouslyRearm);
}

}  // namespace microide::tests
