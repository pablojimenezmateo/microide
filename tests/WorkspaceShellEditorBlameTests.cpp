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

bool RectsIntersect(const SDL_FRect& lhs, const SDL_FRect& rhs) {
  return lhs.x < rhs.x + rhs.w && lhs.x + lhs.w > rhs.x && lhs.y < rhs.y + rhs.h &&
         lhs.y + lhs.h > rhs.y;
}

bool AnyRectIntersects(const std::vector<SDL_FRect>& rects, const SDL_FRect& target) {
  return std::any_of(rects.begin(), rects.end(),
                     [&](const SDL_FRect& rect) { return RectsIntersect(rect, target); });
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

void TestWorkspaceShellEditorBlameLoadsForLargeTrackedFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "large.cpp";
  std::string content;
  for (int i = 0; i < 4205; ++i) {
    content += "int value_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  WriteFile(source, content);

  InitializeGitRepo(root);
  CommitAll(root, "Add large editor blame fixture", "large editor blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(2000, 0);

  const auto overlay = WaitForActiveEditorBlameOverlay(shell);
  Expect(overlay.has_value(),
         "large tracked editors should still expose blame overlays");
  Expect(!overlay->lines.empty(),
         "large tracked editors should publish at least one visible blame line");
  Expect(std::any_of(overlay->lines.begin(), overlay->lines.end(),
                     [](const auto& line) {
                       return line.line_index >= 1999 && line.line_index <= 2001;
                     }),
         "large tracked editor blame should stay near the caret neighborhood");
  Expect(std::any_of(overlay->lines.begin(), overlay->lines.end(),
                     [](const auto& line) {
                       return line.summary == "Add large editor blame fixture";
                     }),
         "large tracked editor blame should keep commit summaries");
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

void TestWorkspaceShellEditorDirtyTransitionRedrawsBlameNeighborhood() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "line 1\nline 2\nline 3\nline 4\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add editor blame redraw fixture", "editor blame redraw fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 0);

  const auto overlay = WaitForActiveEditorBlameOverlay(shell, 3);
  Expect(overlay.has_value() && overlay->lines.size() == 3,
         "clean tracked editor should expose three blame lines before editing");
  (void)shell.ConsumePendingRenderInvalidation();

  SDL_Event event{};
  event.type = SDL_EVENT_TEXT_INPUT;
  const std::string text = "x";
  event.text.text = text.c_str();
  const auto result = shell.HandleEvent(event);

  Expect(result.handled, "editor typing should be handled for blame redraw checks");
  Expect(!result.redraw.full && !result.redraw.rects.empty(),
         "editor typing should stay on the partial redraw path");
  Expect(!WorkspaceShellTestAccess::ActiveEditorBlameOverlay(shell).has_value(),
         "dirty editor buffers should suppress blame immediately after typing");
  Expect(AnyRectIntersects(result.redraw.rects, overlay->lines.front().rect),
         "dirty-state redraw should include the blame line above the caret");
  Expect(AnyRectIntersects(result.redraw.rects, overlay->lines.back().rect),
         "dirty-state redraw should include the blame line below the caret");
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
  Expect(SendMouseMotion(shell, hover_x, hover_y, 0),
         "hovering blame text should request a redraw for the popup");

  const auto popup_rect = WorkspaceShellTestAccess::ActiveEditorBlamePopupRect(shell);
  Expect(popup_rect.has_value(), "hovering blame text should open the blame popup");
  const auto copy_rect = WorkspaceShellTestAccess::ActiveEditorBlamePopupCopyShaRect(shell);
  Expect(copy_rect.has_value(), "blame popup should expose a copy-SHA button");

  const float gap_x = std::max(blame_line.rect.x + 4.0f, popup_rect->x + 4.0f);
  const float gap_y = blame_line.rect.y + blame_line.rect.h +
                      (popup_rect->y - (blame_line.rect.y + blame_line.rect.h)) * 0.5f;
  Expect(SendMouseMotion(shell, gap_x, gap_y, 0),
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
  Expect(SendMouseMotion(shell, copy_x, copy_y, 0),
         "moving onto the blame popup button should request a redraw for button hover");
  Expect(WorkspaceShellTestAccess::ActiveEditorBlamePopupRect(shell).has_value(),
         "moving onto the blame popup button should keep the popup visible");
  Expect(SendMouseDown(shell, copy_x, copy_y, SDL_BUTTON_LEFT),
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
  Expect(SendMouseMotion(shell, hover_x, hover_y, 0),
         "hovering long-summary blame text should open the popup");

  const auto popup_rect = WorkspaceShellTestAccess::ActiveEditorBlamePopupRect(shell);
  Expect(popup_rect.has_value(), "hovering long-summary blame text should open the popup");
  Expect(popup_rect->h > 110.0f,
         "long blame summaries should wrap into a taller popup instead of truncating to one line");
}

}  // namespace

void RegisterWorkspaceShellEditorBlameTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/EditorBlameLoadsForCleanTrackedFile",
          TestWorkspaceShellEditorBlameLoadsForCleanTrackedFile);
  AddTest(tests, "WorkspaceShell/EditorBlameLoadsForLargeTrackedFile",
          TestWorkspaceShellEditorBlameLoadsForLargeTrackedFile);
  AddTest(tests, "WorkspaceShell/EditorBlameHidesForDirtyBufferAndResumesAfterSave",
          TestWorkspaceShellEditorBlameHidesForDirtyBufferAndResumesAfterSave);
  AddTest(tests, "WorkspaceShell/EditorDirtyTransitionRedrawsBlameNeighborhood",
          TestWorkspaceShellEditorDirtyTransitionRedrawsBlameNeighborhood);
  AddTest(tests, "WorkspaceShell/EditorBlameSuppressesNarrowPanes",
          TestWorkspaceShellEditorBlameSuppressesNarrowPanes);
  AddTest(tests, "WorkspaceShell/EditorBlameHoverPopupCopiesCommitSha",
          TestWorkspaceShellEditorBlameHoverPopupCopiesCommitSha);
  AddTest(tests, "WorkspaceShell/EditorBlamePopupWrapsLongSummary",
          TestWorkspaceShellEditorBlamePopupWrapsLongSummary);
}

}  // namespace microide::tests
