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
  Expect(WorkspaceShellTestAccess::StatusMessage(shell) == "Selection copied with context",
         "copy with context should report clipboard feedback");
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
  Expect(WorkspaceShellTestAccess::StatusMessage(shell) == "Blame commit SHA copied",
         "clicking the blame popup copy button should trigger the popup copy action");

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
  AddTest(tests, "WorkspaceShell/ProjectTabsDragReorderToEnd",
          TestWorkspaceShellProjectTabsDragReorderToEnd);
  AddTest(tests, "WorkspaceShell/EditorTabsDragReorderBetweenTabs",
          TestWorkspaceShellEditorTabsDragReorderBetweenTabs);
}

}  // namespace microide::tests
