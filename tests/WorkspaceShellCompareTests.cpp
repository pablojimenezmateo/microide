#include "TestSupport.h"

#include "WorkspaceShellTestAccess.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using microide::workspace::WorkspaceShellTestAccess;

bool RectsIntersect(const SDL_FRect& lhs, const SDL_FRect& rhs) {
  return lhs.x < rhs.x + rhs.w && lhs.x + lhs.w > rhs.x && lhs.y < rhs.y + rhs.h &&
         lhs.y + lhs.h > rhs.y;
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

void TestWorkspaceShellCompareClickTogglesEditablePaneFocus() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare click fixture", "compare click fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare click fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float row_y = surface.rows_y + surface.line_height * 0.5f;
  const float left_x = surface.left_x + 24.0f;
  const float right_x = surface.right_x + 24.0f;

  Expect(compare.right_view_active,
         "compare click fixture should start with the editable pane active");
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(shell, left_x, row_y, SDL_BUTTON_LEFT),
         "clicking the compare left pane should be handled");
  Expect(!compare.right_view_active,
         "clicking the compare left pane should leave the editable pane inactive");

  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(shell, right_x, row_y, SDL_BUTTON_LEFT),
         "clicking the compare right pane should be handled");
  Expect(compare.right_view_active,
         "clicking the compare right pane should reactivate the editable pane");
}

void TestWorkspaceShellCompareWheelScrollsRows() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";

  std::string base_text;
  std::string working_text;
  for (int i = 0; i < 120; ++i) {
    base_text += "base line " + std::to_string(i) + "\n";
    working_text += (i == 60 ? "changed line 60\n"
                             : "base line " + std::to_string(i) + "\n");
  }
  WriteFile(source, base_text);

  InitializeGitRepo(root);
  CommitAll(root, "Add compare wheel fixture", "compare wheel fixture");
  WriteFile(source, working_text);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 420);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare wheel fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(compare.model.rows.size() > static_cast<std::size_t>(surface.visible_rows),
         "compare wheel fixture should overflow the viewport");

  const int before_scroll = compare.scroll_row;
  const float wheel_x = surface.right_x + 24.0f;
  const float wheel_y = surface.rows_y + surface.line_height * 0.5f;
  Expect(WorkspaceShellTestAccess::HandleMouseWheel(shell, wheel_x, wheel_y, -1),
         "scrolling the compare surface should be handled");
  Expect(compare.scroll_row > before_scroll,
         "scrolling the compare surface should advance the visible row");
}

void TestWorkspaceShellCompareHorizontalNavigationInvalidatesEditablePane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare invalidation fixture", "compare invalidation fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare invalidation fixture should open");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const SDL_FRect editable_rect = WorkspaceShellTestAccess::ActiveCompareEditableRect(shell);
  const SDL_FRect left_rect =
      microide::workspace::MakeRect(surface.left_x, surface.rows_y,
                                    surface.gutter_width + surface.left_width,
                                    static_cast<float>(surface.visible_rows) * surface.line_height);

  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = SDLK_RIGHT;
  const auto result = shell.HandleEvent(event);

  Expect(result.handled, "compare horizontal navigation should be handled");
  Expect(!result.redraw.full && result.redraw.rect.has_value(),
         "compare horizontal navigation should stay on a partial redraw path");
  Expect(RectsIntersect(*result.redraw.rect, editable_rect),
         "compare horizontal navigation should repaint the editable pane");
  Expect(!RectsIntersect(*result.redraw.rect, left_rect),
         "compare horizontal navigation should avoid repainting the historical left pane");
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

void TestWorkspaceShellCompareDividerMatchesMarkerWidth() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare divider fixture", "compare divider fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare divider fixture should open");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(surface.divider_width <= std::ceil(WorkspaceShellTestAccess::TextCharWidth(shell)) + 1.0f,
         "compare divider should stay at roughly one glyph wide");
  Expect(surface.right_x - surface.center_x <= surface.divider_width + 0.5f,
         "compare divider should not reserve extra empty space before the right gutter");
}

void TestWorkspaceShellComparePaneResizeKeepsWiderPaneTextVisible() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "abcdefghijklmnopqrstuvwxyz0123456789\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare resize fixture", "compare resize fixture");
  WriteFile(source, "abcdefghijklmnopqrstuvwxyz9876543210\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare resize fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.divider_fraction = 0.75f;
  const auto left_wide = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(left_wide.left_visible_columns > left_wide.right_visible_columns,
         "widening the left compare pane should preserve more visible text on the left");

  compare.divider_fraction = 0.25f;
  const auto right_wide = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(right_wide.right_visible_columns > right_wide.left_visible_columns,
         "widening the right compare pane should preserve more visible text on the right");
}

}  // namespace

void RegisterWorkspaceShellCompareTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/WorkingTreeCompareIsEditableAndSaves",
          TestWorkspaceShellWorkingTreeCompareIsEditableAndSaves);
  AddTest(tests, "WorkspaceShell/CompareClickTogglesEditablePaneFocus",
          TestWorkspaceShellCompareClickTogglesEditablePaneFocus);
  AddTest(tests, "WorkspaceShell/CompareWheelScrollsRows",
          TestWorkspaceShellCompareWheelScrollsRows);
  AddTest(tests, "WorkspaceShell/CompareHorizontalNavigationInvalidatesEditablePane",
          TestWorkspaceShellCompareHorizontalNavigationInvalidatesEditablePane);
  AddTest(tests, "WorkspaceShell/CompareBlameLoadsForWorkingTreePane",
          TestWorkspaceShellCompareBlameLoadsForWorkingTreePane);
  AddTest(tests, "WorkspaceShell/MergeBlameLoadsForResultPane",
          TestWorkspaceShellMergeBlameLoadsForResultPane);
  AddTest(tests, "WorkspaceShell/CompareTabUsesFilenameOnlyLabelAndTooltip",
          TestWorkspaceShellCompareTabUsesFilenameOnlyLabelAndTooltip);
  AddTest(tests, "WorkspaceShell/MergeTabUsesFilenameOnlyLabelAndTooltip",
          TestWorkspaceShellMergeTabUsesFilenameOnlyLabelAndTooltip);
  AddTest(tests, "WorkspaceShell/CompareDividerMatchesMarkerWidth",
          TestWorkspaceShellCompareDividerMatchesMarkerWidth);
  AddTest(tests, "WorkspaceShell/ComparePaneResizeKeepsWiderPaneTextVisible",
          TestWorkspaceShellComparePaneResizeKeepsWiderPaneTextVisible);
}

}  // namespace microide::tests
