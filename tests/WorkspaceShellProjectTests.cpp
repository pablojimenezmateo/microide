#include "TestSupport.h"

#include "WorkspaceShellTestAccess.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using microide::workspace::WorkspaceShellTestAccess;

std::string EscapedRepoPath(const std::filesystem::path& repo_path) {
  return ShellEscape(repo_path.string());
}

void InitializeGitRepo(const std::filesystem::path& repo_path) {
  const std::string escaped_repo = EscapedRepoPath(repo_path);
  RequireCommandSuccess(
      "git -c init.defaultBranch=main init '" + escaped_repo + "' >/dev/null 2>/dev/null",
      "git init");
  RequireCommandSuccess(
      "git -C '" + escaped_repo + "' config user.name 'Microide Tests' >/dev/null 2>/dev/null",
      "git config user.name");
  RequireCommandSuccess(
      "git -C '" + escaped_repo +
          "' config user.email 'microide-tests@example.com' >/dev/null 2>/dev/null",
      "git config user.email");
}

void CommitAll(const std::filesystem::path& repo_path,
               std::string_view message,
               std::string_view context) {
  const std::string escaped_repo = EscapedRepoPath(repo_path);
  RequireCommandSuccess("git -C '" + escaped_repo + "' add . >/dev/null 2>/dev/null",
                        std::string(context) + " add");
  RequireCommandSuccess(
      "git -C '" + escaped_repo + "' commit -m '" + std::string(message) +
          "' >/dev/null 2>/dev/null",
      std::string(context) + " commit");
}

std::optional<microide::editor::EditorBlameOverlay> WaitForActiveEditorBlameOverlay(
    WorkspaceShell& shell,
    std::size_t minimum_line_count = 1) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto overlay = WorkspaceShellTestAccess::ActiveEditorBlameOverlay(shell);
    if (overlay.has_value() && overlay->lines.size() >= minimum_line_count) {
      return overlay;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return WorkspaceShellTestAccess::ActiveEditorBlameOverlay(shell);
}

std::optional<microide::editor::EditorBlameOverlay> WaitForActiveCompareBlameOverlay(
    WorkspaceShell& shell,
    std::size_t minimum_line_count = 1) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto overlay = WorkspaceShellTestAccess::ActiveCompareBlameOverlay(shell);
    if (overlay.has_value() && overlay->lines.size() >= minimum_line_count) {
      return overlay;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return WorkspaceShellTestAccess::ActiveCompareBlameOverlay(shell);
}

std::optional<microide::editor::EditorBlameOverlay> WaitForActiveMergeBlameOverlay(
    WorkspaceShell& shell,
    std::size_t minimum_line_count = 1) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto overlay = WorkspaceShellTestAccess::ActiveMergeBlameOverlay(shell);
    if (overlay.has_value() && overlay->lines.size() >= minimum_line_count) {
      return overlay;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return WorkspaceShellTestAccess::ActiveMergeBlameOverlay(shell);
}

bool ExecuteCommand(WorkspaceShell& shell, std::string_view command) {
  return WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_E, SDL_KMOD_CTRL) &&
         WorkspaceShellTestAccess::HandleTextInput(shell, command) &&
         WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_RETURN, SDL_KMOD_NONE);
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

void TestWorkspaceShellUnknownCommandKeepsPromptOpenWithFeedback() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::ResetProjectScopedState(shell, true);

  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_E, SDL_KMOD_CTRL),
         "Ctrl+E should open the command prompt");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "bogus-command"),
         "text input should populate the command prompt");
  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should attempt to execute the typed command");

  Expect(WorkspaceShellTestAccess::CommandMode(shell),
         "unknown commands should keep the command prompt open");
  Expect(WorkspaceShellTestAccess::CommandPromptStatusText(shell) == "Unknown command: bogus-command",
         "unknown commands should report an explicit prompt error");
}

void TestWorkspaceShellCommandReportsMissingProjectInsteadOfSilentNoOp() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::ResetProjectScopedState(shell, true);

  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_E, SDL_KMOD_CTRL),
         "Ctrl+E should open the command prompt before the missing-project test");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "search"),
         "text input should populate the missing-project command");
  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_RETURN, SDL_KMOD_NONE),
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

  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_E, SDL_KMOD_CTRL),
         "Ctrl+E should open the command prompt before the open-path test");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "open"),
         "text input should populate the open command");
  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_RETURN, SDL_KMOD_NONE),
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

  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_E, SDL_KMOD_CTRL),
         "Ctrl+E should open the command prompt before cycling projects");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "project-prev"),
         "text input should populate the project-prev command");
  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should execute the project-prev command");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_b.lexically_normal(),
         "project-prev should activate the previous project tab");

  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_E, SDL_KMOD_CTRL),
         "Ctrl+E should reopen the command prompt for project-next");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "project-next"),
         "text input should populate the project-next command");
  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should execute the project-next command");
  Expect(WorkspaceShellTestAccess::ProjectRoot(shell) == root_c.lexically_normal(),
         "project-next should activate the next project tab");
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

void TestWorkspaceShellGlobalCommandsApplyTypedRequests() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);

  Expect(ExecuteCommand(shell, "ui-scale 125%"),
         "ui-scale should execute with a parsed numeric scale");
  Expect(std::fabs(WorkspaceShellTestAccess::UiScale(shell) - 1.25f) < 0.001f,
         "ui-scale should apply the parsed scale");

  Expect(ExecuteCommand(shell, "soft-tabs on"),
         "soft-tabs should execute with a typed boolean request");
  Expect(WorkspaceShellTestAccess::SoftTabsEnabled(shell),
         "soft-tabs should enable soft tabs for editor preferences");

  Expect(ExecuteCommand(shell, "focus panel"),
         "focus should execute with a typed focus target");
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell),
         "focus panel should move focus to the bottom panel when available");
}

void TestWorkspaceShellCtrlNOpensUntitledTab() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "hello\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_N, SDL_KMOD_CTRL),
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

  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
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

  Expect(WorkspaceShellTestAccess::HandleKeyEvent(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "Escape should close the welcome overlay");
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
         "Escape should dismiss the welcome overlay");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell),
         "closing the welcome overlay should restore editor focus when no sidebar is visible");
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
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(shell, click_x, click_y, SDL_BUTTON_LEFT),
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

  Expect(tree_contains_path(source),
         "reselecting the open file tab should re-expand its ancestors in the tree");
  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == source.lexically_normal(),
         "reselecting the open file tab should reselect the file in the tree");
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

void TestWorkspaceShellEditorRightClickOpensEditContextMenu() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  std::filesystem::create_directories(root);
  WriteFile(source, "int main() {}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.button = SDL_BUTTON_RIGHT;
  event.button.x = 320;
  event.button.y = 140;

  Expect(shell.HandleEvent(event),
         "right-clicking the editor should be handled");
  Expect(WorkspaceShellTestAccess::MenuBarOpen(shell),
         "right-clicking the editor should open a popup menu");
  Expect(WorkspaceShellTestAccess::EditMenuOpen(shell),
         "right-clicking the editor should open the edit popup as a context menu");
}

void TestWorkspaceShellEditorBlameLoadsForCleanTrackedFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\nint beta() {\n  return 2;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add editor blame fixture", "editor blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(2, 0);

  const auto overlay = WaitForActiveEditorBlameOverlay(shell, 3);
  Expect(overlay.has_value(), "clean tracked editor should eventually expose blame overlay");
  Expect(overlay->lines.size() == 3,
         "editor blame overlay should stay focused on the caret line and adjacent rows");
  Expect(overlay->lines[0].line_index == 1 && overlay->lines[1].line_index == 2 &&
             overlay->lines[2].line_index == 3,
         "editor blame overlay should only include the caret line, above, and below");
  Expect(overlay->lines[1].author == "Microide Tests",
         "editor blame overlay should keep the blame author metadata");
  Expect(overlay->lines[1].summary == "Add editor blame fixture",
         "editor blame overlay should keep the blame summary metadata");

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  WorkspaceShellTestAccess::ActiveEditor(shell).SetViewportSize(metrics.visible_rows, metrics.visible_columns);
  const auto layout = WorkspaceShellTestAccess::ActiveEditor(shell).VisibleLineLayout(2);
  const float expected_x = metrics.text_x +
                           static_cast<float>(layout.visual_columns + 8) *
                               WorkspaceShellTestAccess::TextCharWidth(shell);
  Expect(std::fabs(overlay->lines[1].rect.x - expected_x) < 0.5f,
         "editor blame overlay should anchor eight columns after the visible line end");
}

void TestWorkspaceShellEditorBlameHidesForDirtyBufferAndResumesAfterSave() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int main() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add editor blame fixture", "editor blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(WaitForActiveEditorBlameOverlay(shell).has_value(),
         "clean tracked editor should load blame before dirty-state checks");

  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("// dirty\n");
  Expect(!WorkspaceShellTestAccess::ActiveEditorBlameOverlay(shell).has_value(),
         "dirty editor buffer should suppress blame immediately");

  Expect(WorkspaceShellTestAccess::SaveTab(shell, 0),
         "saving the dirty editor should succeed");

  const auto overlay = WaitForActiveEditorBlameOverlay(shell);
  Expect(overlay.has_value(),
         "saved tracked editor should resume blame after the file reaches disk");
  Expect(!overlay->lines.empty(), "saved tracked editor should publish visible blame lines");
  Expect(std::any_of(overlay->lines.begin(), overlay->lines.end(),
                     [](const auto& line) { return line.text == "Saved changes"; }),
         "saved tracked editor should still mark working-tree-only lines as saved changes");
}

void TestWorkspaceShellEditorBlameSuppressesNarrowPanes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int main() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add editor blame fixture", "editor blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 420, 320);

  Expect(!WorkspaceShellTestAccess::ActiveEditorBlameOverlay(shell).has_value(),
         "narrow editor panes should suppress blame instead of stealing code width");
}

void TestWorkspaceShellEditorBlameHoverPopupCopiesCommitSha() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "line 1\nline 2\nline 3\nline 4\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add editor blame fixture", "editor blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 0);

  const auto overlay = WaitForActiveEditorBlameOverlay(shell, 3);
  Expect(overlay.has_value() && overlay->lines.size() == 3,
         "hover popup fixture should have visible inline blame");

  WorkspaceShellTestAccess::SetVisibleEditorBlameOverlay(shell, overlay);
  const auto& blame_line = overlay->lines[1];
  const float hover_x = blame_line.rect.x + 4.0f;
  const float hover_y = blame_line.rect.y + blame_line.rect.h * 0.5f;
  Expect(WorkspaceShellTestAccess::HandleMouseMotion(shell, hover_x, hover_y, 0),
         "hovering blame text should request a redraw for the popup");

  const auto popup_rect = WorkspaceShellTestAccess::ActiveEditorBlamePopupRect(shell);
  Expect(popup_rect.has_value(), "hovering blame text should open the blame popup");
  const auto copy_rect = WorkspaceShellTestAccess::ActiveEditorBlamePopupCopyShaRect(shell);
  Expect(copy_rect.has_value(), "blame popup should expose a copy-SHA button");

  const float gap_x = std::max(blame_line.rect.x + 4.0f, popup_rect->x + 4.0f);
  const float gap_y =
      blame_line.rect.y + blame_line.rect.h + (popup_rect->y - (blame_line.rect.y + blame_line.rect.h)) * 0.5f;
  Expect(WorkspaceShellTestAccess::HandleMouseMotion(shell, gap_x, gap_y, 0),
         "moving from blame text toward the popup should keep the UI dirty for hover updates");
  Expect(WorkspaceShellTestAccess::ActiveEditorBlamePopupRect(shell).has_value(),
         "moving from blame text toward the popup should keep the popup visible");

  std::string copied_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        copied_text = std::string(text);
        return true;
      });

  const float copy_x = copy_rect->x + copy_rect->w * 0.5f;
  const float copy_y = copy_rect->y + copy_rect->h * 0.5f;
  Expect(WorkspaceShellTestAccess::HandleMouseMotion(shell, copy_x, copy_y, 0),
         "moving onto the blame popup button should request a redraw for button hover");
  Expect(WorkspaceShellTestAccess::ActiveEditorBlamePopupRect(shell).has_value(),
         "moving onto the blame popup button should keep the popup visible");
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(shell, copy_x, copy_y, SDL_BUTTON_LEFT),
         "clicking the blame popup copy button should be handled");

  Expect(copied_text == blame_line.commit_id,
         "clicking the blame popup copy button should copy the full commit SHA");
}

void TestWorkspaceShellEditorBlamePopupWrapsLongSummary() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "line 1\nline 2\nline 3\nline 4\n");

  InitializeGitRepo(root);
  CommitAll(root,
            "This is a deliberately long blame summary that should wrap inside the popup instead of truncating too early",
            "editor blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 0);

  const auto overlay = WaitForActiveEditorBlameOverlay(shell, 3);
  Expect(overlay.has_value() && overlay->lines.size() == 3,
         "long-summary popup fixture should have visible inline blame");

  WorkspaceShellTestAccess::SetVisibleEditorBlameOverlay(shell, overlay);
  const auto& blame_line = overlay->lines[1];
  const float hover_x = blame_line.rect.x + 4.0f;
  const float hover_y = blame_line.rect.y + blame_line.rect.h * 0.5f;
  Expect(WorkspaceShellTestAccess::HandleMouseMotion(shell, hover_x, hover_y, 0),
         "hovering long-summary blame text should open the popup");

  const auto popup_rect = WorkspaceShellTestAccess::ActiveEditorBlamePopupRect(shell);
  Expect(popup_rect.has_value(), "hovering long-summary blame text should open the popup");
  Expect(popup_rect->h > 110.0f,
         "long blame summaries should wrap into a taller popup instead of truncating to one line");
}

void TestWorkspaceShellGitSidebarRefreshPreservesActiveEditorBlameCache() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\nint beta() {\n  return 2;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add editor blame fixture", "editor blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(2, 0);

  const auto loaded_overlay = WaitForActiveEditorBlameOverlay(shell, 3);
  Expect(loaded_overlay.has_value() && loaded_overlay->lines.size() == 3,
         "refresh-preservation fixture should start with loaded blame lines");

  WorkspaceShellTestAccess::RefreshGitSidebar(shell);

  const auto refreshed_overlay = WorkspaceShellTestAccess::ActiveEditorBlameOverlay(shell);
  Expect(refreshed_overlay.has_value() && refreshed_overlay->lines.size() == 3,
         "refreshing the git sidebar should not flush an unrelated active editor blame cache");
  Expect(refreshed_overlay->lines[1].author == "Microide Tests",
         "refreshing the git sidebar should preserve blame metadata for the active editor");
}

void TestWorkspaceShellGitSidebarCompactButtonsExposeHoverTooltips() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "deep" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add git sidebar tooltip fixture", "git sidebar tooltip fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);

  Expect(WorkspaceShellTestAccess::GitSidebarEntries(shell).size() == 1,
         "git sidebar tooltip fixture should expose a single modified entry");

  const auto top_action_rects = WorkspaceShellTestAccess::GitSidebarTopActionRects(shell);
  WorkspaceShellTestAccess::HandleMouseMotion(
      shell, top_action_rects[0].x + top_action_rects[0].w * 0.5f,
      top_action_rects[0].y + top_action_rects[0].h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell).empty(),
         "hovering the full-width stage-all button should not show a tooltip");

  const auto action_rects = WorkspaceShellTestAccess::GitSidebarEntryActionRects(shell, 0);
  WorkspaceShellTestAccess::HandleMouseMotion(shell, action_rects[0].x + action_rects[0].w * 0.5f,
                                              action_rects[0].y + action_rects[0].h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell) == "Stage",
         "hovering the compact stage button should expose the full action name");

  WorkspaceShellTestAccess::HandleMouseMotion(shell, action_rects[0].x - 2.0f,
                                              action_rects[0].y + action_rects[0].h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell) == "Stage",
         "stage button hover should tolerate a small hitbox miss");

  WorkspaceShellTestAccess::HandleMouseMotion(shell, action_rects[1].x + action_rects[1].w * 0.5f,
                                              action_rects[1].y + action_rects[1].h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell) == "Discard",
         "hovering the compact discard button should expose the full action name");

  Expect(WorkspaceShellTestAccess::StageAllGitSidebarEntries(shell),
         "staging the tooltip fixture should succeed");
  const auto staged_action_rects = WorkspaceShellTestAccess::GitSidebarEntryActionRects(shell, 0);
  WorkspaceShellTestAccess::HandleMouseMotion(
      shell, staged_action_rects[0].x + staged_action_rects[0].w * 0.5f,
      staged_action_rects[0].y + staged_action_rects[0].h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell) == "Unstage",
         "hovering the compact unstage button should expose the full action name");
}

void TestWorkspaceShellWorkingTreeCompareIsEditableAndSaves() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare edit fixture", "compare edit fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.right_editable,
         "working-tree comparison should expose an editable current-state pane");
  Expect(compare.right_view_active,
         "working-tree comparison should focus the editable current-state pane");

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "// note "),
         "text input should edit the compare current-state pane");
  Expect(compare.right_viewport.dirty(),
         "editing the compare current-state pane should mark the tab dirty");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, 0),
         "saving the compare tab should write the current-state buffer");
  Expect(!compare.right_viewport.dirty(),
         "saving the compare tab should clear the dirty state");
  Expect(ReadFile(source).rfind("// note ", 0) == 0,
         "saving the compare tab should persist the edited current-state text");
}

void TestWorkspaceShellCompareBlameLoadsForWorkingTreePane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "line 1\nline 2\nline 3\nline 4\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare blame fixture", "compare blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.right_viewport.MoveCursorTo(1, 0);

  const auto overlay = WaitForActiveCompareBlameOverlay(shell, 3);
  Expect(overlay.has_value(),
         "clean working-tree comparison should eventually expose compare blame");
  Expect(overlay->lines.size() == 3,
         "compare blame should stay focused on the caret line and adjacent rows");
  Expect(overlay->lines[1].author == "Microide Tests",
         "compare blame should keep the blame author metadata");
  Expect(overlay->lines[1].summary == "Add compare blame fixture",
         "compare blame should keep the blame summary metadata");
}

void TestWorkspaceShellMergeBlameLoadsForResultPane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const std::filesystem::path base = temp_dir.path() / "base.cpp";
  const std::filesystem::path incoming = temp_dir.path() / "incoming.cpp";
  WriteFile(source, "line 1\ncurrent line\nline 3\nline 4\n");
  WriteFile(base, "line 1\nbase line\nline 3\nline 4\n");
  WriteFile(incoming, "line 1\nincoming line\nline 3\nline 4\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add merge blame fixture", "merge blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, source, source),
         "merge editor should open");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  merge.result_viewport.MoveCursorTo(1, 0);

  const auto overlay = WaitForActiveMergeBlameOverlay(shell, 3);
  Expect(overlay.has_value(),
         "clean merge result pane should eventually expose blame");
  Expect(overlay->lines.size() == 3,
         "merge blame should stay focused on the caret line and adjacent rows");
  Expect(overlay->lines[1].author == "Microide Tests",
         "merge blame should keep the blame author metadata");
  Expect(overlay->lines[1].summary == "Add merge blame fixture",
         "merge blame should keep the blame summary metadata");
}

void TestWorkspaceShellCompareTabUsesFilenameOnlyLabelAndTooltip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "nested" / "compare.txt";
  WriteFile(source, "zero\none\ntwo\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare tab fixture", "compare tab fixture");
  WriteFile(source, "zero\none changed\ntwo changed\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open for compact-tab fixture");

  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 0) == "compare.txt",
         "compare tabs should display only the filename");
  Expect(WorkspaceShellTestAccess::TabTooltipLabel(shell, 0) == "src/nested/compare.txt",
         "compare tab tooltip should expose the full relative path");
  const std::string breadcrumb = WorkspaceShellTestAccess::BreadcrumbLabel(shell);
  Expect(breadcrumb.find("src/nested/compare.txt") != std::string::npos,
         "active compare breadcrumbs should keep the relative path");
  Expect(breadcrumb.find("HEAD -> Working tree") != std::string::npos,
         "active compare breadcrumbs should keep the compare refs");
}

void TestWorkspaceShellMergeTabUsesFilenameOnlyLabelAndTooltip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "src" / "result.txt";
  WriteFile(base, "top\nbase\nbottom\n");
  WriteFile(incoming, "top\nincoming\nbottom\n");
  WriteFile(current, "top\ncurrent\nbottom\n");
  WriteFile(output, "top\ncurrent\nbottom\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for compact-tab fixture");

  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 0) == "result.txt",
         "merge tabs should display only the output filename");
  Expect(WorkspaceShellTestAccess::TabTooltipLabel(shell, 0) == "src/result.txt",
         "merge tab tooltip should expose the full relative path");
  Expect(WorkspaceShellTestAccess::BreadcrumbLabel(shell).find("src/result.txt") != std::string::npos,
         "active merge breadcrumbs should keep the relative path");
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
  WorkspaceShellTestAccess::HandleMouseMotion(shell, tab_rect.x + tab_rect.w * 0.5f,
                                              tab_rect.y + tab_rect.h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredTabTooltipLabel(shell) == "src/deep/main.cpp",
         "hovering a tab should expose the full relative path tooltip");
}

void TestWorkspaceShellMenuBarOmitsDuplicateTerminalAndHelpMenus() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const std::vector<std::string> labels = WorkspaceShellTestAccess::VisibleMenuBarLabels(shell);
  Expect(std::count(labels.begin(), labels.end(), "Terminal") == 1,
         "menu bar should show only one Terminal menu");
  Expect(std::find(labels.begin(), labels.end(), "Help") == labels.end(),
         "menu bar should omit the removed Help menu");
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
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(
             shell, source_rect.x + source_rect.w * 0.5f, source_rect.y + source_rect.h * 0.5f,
             SDL_BUTTON_LEFT),
         "dragging should start from a project tab press");

  const SDL_FRect last_rect = WorkspaceShellTestAccess::ProjectTabRect(shell, 2);
  const float drop_x = last_rect.x + last_rect.w + 12.0f;
  const float drop_y = last_rect.y + last_rect.h * 0.5f;
  Expect(WorkspaceShellTestAccess::HandleMouseMotion(shell, drop_x, drop_y, SDL_BUTTON_LMASK),
         "dragging across the project tab strip should be handled");
  Expect(WorkspaceShellTestAccess::HandleMouseButtonUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT),
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
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(
             shell, source_rect.x + source_rect.w * 0.5f, source_rect.y + source_rect.h * 0.5f,
             SDL_BUTTON_LEFT),
         "dragging should start from an editor tab press");

  const SDL_FRect third_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 2);
  const float drop_x = third_rect.x + 1.0f;
  const float drop_y = third_rect.y + third_rect.h * 0.5f;
  Expect(WorkspaceShellTestAccess::HandleMouseMotion(shell, drop_x, drop_y, SDL_BUTTON_LMASK),
         "dragging across editor tabs should be handled");
  Expect(WorkspaceShellTestAccess::HandleMouseButtonUp(shell, drop_x, drop_y, SDL_BUTTON_LEFT),
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

}  // namespace

void RegisterWorkspaceShellProjectTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/ProjectOpenMenuUsesNativePickerSelection",
          TestWorkspaceShellProjectOpenMenuUsesNativePickerSelection);
  AddTest(tests, "WorkspaceShell/ProjectOpenCommandUsesNativePickerAtActiveProjectRoot",
          TestWorkspaceShellProjectOpenCommandUsesNativePickerAtActiveProjectRoot);
  AddTest(tests, "WorkspaceShell/ProjectOpenMenuFallsBackToTypedPathWhenNativePickerFails",
          TestWorkspaceShellProjectOpenMenuFallsBackToTypedPathWhenNativePickerFails);
  AddTest(tests, "WorkspaceShell/UnknownCommandKeepsPromptOpenWithFeedback",
          TestWorkspaceShellUnknownCommandKeepsPromptOpenWithFeedback);
  AddTest(tests, "WorkspaceShell/CommandReportsMissingProjectInsteadOfSilentNoOp",
          TestWorkspaceShellCommandReportsMissingProjectInsteadOfSilentNoOp);
  AddTest(tests, "WorkspaceShell/OpenCommandRequiresPath",
          TestWorkspaceShellOpenCommandRequiresPath);
  AddTest(tests, "WorkspaceShell/ProjectNextAndPrevCommandsCycleProjects",
          TestWorkspaceShellProjectNextAndPrevCommandsCycleProjects);
  AddTest(tests, "WorkspaceShell/SidebarWidthCommandParsesTypedRequests",
          TestWorkspaceShellSidebarWidthCommandParsesTypedRequests);
  AddTest(tests, "WorkspaceShell/MergeCommandResolvesRelativePaths",
          TestWorkspaceShellMergeCommandResolvesRelativePaths);
  AddTest(tests, "WorkspaceShell/TabMoveCommandSupportsRelativeOffsets",
          TestWorkspaceShellTabMoveCommandSupportsRelativeOffsets);
  AddTest(tests, "WorkspaceShell/GotoAndJumpCommandsUseTypedNavigationRequests",
          TestWorkspaceShellGotoAndJumpCommandsUseTypedNavigationRequests);
  AddTest(tests, "WorkspaceShell/GlobalCommandsApplyTypedRequests",
          TestWorkspaceShellGlobalCommandsApplyTypedRequests);
  AddTest(tests, "WorkspaceShell/CtrlNOpensUntitledTab",
          TestWorkspaceShellCtrlNOpensUntitledTab);
  AddTest(tests, "WorkspaceShell/FilesShortcutEscapeRestoresSidebarFocus",
          TestWorkspaceShellFilesShortcutEscapeRestoresSidebarFocus);
  AddTest(tests, "WorkspaceShell/FilesShortcutEscapeRestoresEditorFocusOnWelcome",
          TestWorkspaceShellFilesShortcutEscapeRestoresEditorFocusOnWelcome);
  AddTest(tests, "WorkspaceShell/OverlayOutsideClickRestoresPrimaryFocus",
          TestWorkspaceShellOverlayOutsideClickRestoresPrimaryFocus);
  AddTest(tests, "WorkspaceShell/TreeCollapseAllowsOpenDescendantsAndReselectReveal",
          TestWorkspaceShellTreeCollapseAllowsOpenDescendantsAndReselectReveal);
  AddTest(tests, "WorkspaceShell/CopySelectionWithContextUsesRelativePathAndLineRange",
          TestWorkspaceShellCopySelectionWithContextUsesRelativePathAndLineRange);
  AddTest(tests, "WorkspaceShell/EditorRightClickOpensEditContextMenu",
          TestWorkspaceShellEditorRightClickOpensEditContextMenu);
  AddTest(tests, "WorkspaceShell/EditorBlameLoadsForCleanTrackedFile",
          TestWorkspaceShellEditorBlameLoadsForCleanTrackedFile);
  AddTest(tests, "WorkspaceShell/EditorBlameHidesForDirtyBufferAndResumesAfterSave",
          TestWorkspaceShellEditorBlameHidesForDirtyBufferAndResumesAfterSave);
  AddTest(tests, "WorkspaceShell/EditorBlameSuppressesNarrowPanes",
          TestWorkspaceShellEditorBlameSuppressesNarrowPanes);
  AddTest(tests, "WorkspaceShell/EditorBlameHoverPopupCopiesCommitSha",
          TestWorkspaceShellEditorBlameHoverPopupCopiesCommitSha);
  AddTest(tests, "WorkspaceShell/EditorBlamePopupWrapsLongSummary",
          TestWorkspaceShellEditorBlamePopupWrapsLongSummary);
  AddTest(tests, "WorkspaceShell/GitSidebarRefreshPreservesActiveEditorBlameCache",
          TestWorkspaceShellGitSidebarRefreshPreservesActiveEditorBlameCache);
  AddTest(tests, "WorkspaceShell/GitSidebarCompactButtonsExposeHoverTooltips",
          TestWorkspaceShellGitSidebarCompactButtonsExposeHoverTooltips);
  AddTest(tests, "WorkspaceShell/WorkingTreeCompareIsEditableAndSaves",
          TestWorkspaceShellWorkingTreeCompareIsEditableAndSaves);
  AddTest(tests, "WorkspaceShell/CompareBlameLoadsForWorkingTreePane",
          TestWorkspaceShellCompareBlameLoadsForWorkingTreePane);
  AddTest(tests, "WorkspaceShell/MergeBlameLoadsForResultPane",
          TestWorkspaceShellMergeBlameLoadsForResultPane);
  AddTest(tests, "WorkspaceShell/CompareTabUsesFilenameOnlyLabelAndTooltip",
          TestWorkspaceShellCompareTabUsesFilenameOnlyLabelAndTooltip);
  AddTest(tests, "WorkspaceShell/MergeTabUsesFilenameOnlyLabelAndTooltip",
          TestWorkspaceShellMergeTabUsesFilenameOnlyLabelAndTooltip);
  AddTest(tests, "WorkspaceShell/HoveredTabShowsRelativePathTooltip",
          TestWorkspaceShellHoveredTabShowsRelativePathTooltip);
  AddTest(tests, "WorkspaceShell/MenuBarOmitsDuplicateTerminalAndHelpMenus",
          TestWorkspaceShellMenuBarOmitsDuplicateTerminalAndHelpMenus);
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
}

}  // namespace microide::tests
