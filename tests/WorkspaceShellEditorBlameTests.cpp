#include "TestSupport.h"

#include "workspace/shell/WorkspaceShellTestAccess.h"

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

std::optional<microide::editor::EditorBlameOverlay> WaitForActiveEditorBlameOverlay(
    WorkspaceShell& shell,
    std::size_t minimum_line_count = 1) {
  WaitUntil([&shell, minimum_line_count]() {
    const auto overlay = WorkspaceShellTestAccess::ActiveEditorBlameOverlay(shell);
    return overlay.has_value() && overlay->lines.size() >= minimum_line_count;
  }, std::chrono::seconds(2), std::chrono::milliseconds(10));
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

  const auto overlay = WaitForActiveEditorBlameOverlay(shell, 1);
  Expect(overlay.has_value(), "clean tracked editor should eventually expose blame overlay");
  Expect(overlay->lines.size() == 1,
         "editor blame overlay should annotate the caret line only, like VSCode/GitLens");
  Expect(overlay->lines[0].line_index == 2,
         "editor blame overlay should only include the caret line");
  Expect(overlay->lines[0].author == "Microide Tests",
         "editor blame overlay should keep the blame author metadata");
  Expect(overlay->lines[0].summary == "Add editor blame fixture",
         "editor blame overlay should keep the blame summary metadata");

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(shell);
  WorkspaceShellTestAccess::ActiveEditor(shell).SetViewportSize(metrics.visible_rows, metrics.visible_columns);
  const auto layout = WorkspaceShellTestAccess::ActiveEditor(shell).VisibleLineLayout(2);
  const float expected_x = metrics.text_x +
                           static_cast<float>(layout.visual_columns + 8) *
                               WorkspaceShellTestAccess::TextCharWidth(shell);
  Expect(std::fabs(overlay->lines[0].rect.x - expected_x) < 0.5f,
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

void TestWorkspaceShellEditorBlameTracksStickyScrollYOffset() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "sticky.cpp";
  std::string content;
  for (int i = 0; i < 60; ++i) {
    content += "int filler_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  content += "void outer() {\n";
  content += "  if (ready) {\n";
  content += "    if (nested) {\n";
  for (int i = 0; i < 25; ++i) {
    content += "      int nested_fill_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  content += "      int blame_target = 1;\n";
  content += "      int blame_neighbor = 2;\n";
  content += "    }\n";
  content += "  }\n";
  content += "}\n";
  WriteFile(source, content);

  InitializeGitRepo(root);
  CommitAll(root, "Add sticky blame fixture", "editor blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 220);
  const std::size_t target_line = 88;
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(target_line, 0);
  (void)WorkspaceShellTestAccess::ActiveEditorRenderMetrics(shell);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(target_line, 0);

  const auto overlay = WaitForActiveEditorBlameOverlay(shell);
  Expect(overlay.has_value() && !overlay->lines.empty(),
         "sticky-scroll blame fixture should expose at least one visible blame line");

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorRenderMetrics(shell);
  Expect(metrics.sticky_scroll_rows > 0,
         "sticky-scroll blame fixture should activate at least one sticky row");
  const auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  const std::size_t expected_row = viewport.VisualRowForLine(target_line) - viewport.scroll_line();
  const float expected_y =
      metrics.first_line_y + static_cast<float>(expected_row) * metrics.line_height;
  const auto it = std::find_if(overlay->lines.begin(), overlay->lines.end(),
                               [&](const auto& line) { return line.line_index == target_line; });
  Expect(it != overlay->lines.end(),
         "sticky-scroll blame fixture should include the caret line overlay");
  Expect(std::fabs(it->rect.y - expected_y) < 0.5f,
         "inline blame should share the same sticky-scroll-adjusted y-offset as the editor row");
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

  // Inline blame annotates the caret line only, so park the caret on the line
  // that exists in the working tree but not in HEAD to observe its marker.
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);

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

  const auto overlay = WaitForActiveEditorBlameOverlay(shell, 1);
  Expect(overlay.has_value() && overlay->lines.size() == 1,
         "clean tracked editor should expose the caret blame line before editing");
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
  Expect(AnyRectCovers(result.redraw.rects, overlay->lines.front().rect),
         "dirty-state redraw should include the caret blame line it just erased");
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

  const auto overlay = WaitForActiveEditorBlameOverlay(shell, 1);
  Expect(overlay.has_value() && overlay->lines.size() == 1,
         "hover popup fixture should have visible inline blame");

  WorkspaceShellTestAccess::SetVisibleEditorBlameOverlay(shell, overlay);
  const auto& blame_line = overlay->lines[0];
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

  const auto overlay = WaitForActiveEditorBlameOverlay(shell, 1);
  Expect(overlay.has_value() && overlay->lines.size() == 1,
         "long-summary popup fixture should have visible inline blame");

  WorkspaceShellTestAccess::SetVisibleEditorBlameOverlay(shell, overlay);
  const auto& blame_line = overlay->lines[0];
  const float hover_x = blame_line.rect.x + 4.0f;
  const float hover_y = blame_line.rect.y + blame_line.rect.h * 0.5f;
  Expect(SendMouseMotion(shell, hover_x, hover_y, 0),
         "hovering long-summary blame text should open the popup");

  const auto popup_rect = WorkspaceShellTestAccess::ActiveEditorBlamePopupRect(shell);
  Expect(popup_rect.has_value(), "hovering long-summary blame text should open the popup");
  Expect(popup_rect->h > 110.0f,
         "long blame summaries should wrap into a taller popup instead of truncating to one line");
}

// Hover wrapping is memoized (it runs several times per frame for as long as a
// card is on screen). A memo is only ever wrong by being stale, so what needs
// pinning is that every key component actually participates: text, wrap width,
// line cap, and the font metrics generation.
void TestWorkspaceShellHoverWrapMemoKeysOnEveryInput() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  const std::string_view text =
      "the quick brown fox jumps over the lazy dog and keeps on running past the end";

  const auto narrow = WorkspaceShellTestAccess::WrapHoverPopupText(shell, text, 120.0f, 8);
  const auto narrow_again = WorkspaceShellTestAccess::WrapHoverPopupText(shell, text, 120.0f, 8);
  Expect(!narrow.empty(), "the fixture text should wrap to at least one line");
  Expect(narrow == narrow_again, "a repeated wrap must return the same lines");

  // Width is part of the key: a wider card fits more per line.
  const auto wide = WorkspaceShellTestAccess::WrapHoverPopupText(shell, text, 600.0f, 8);
  Expect(wide != narrow, "changing the wrap width must not return the narrow cache entry");
  Expect(wide.size() < narrow.size(), "a wider card should need fewer lines");

  // Line cap is part of the key.
  const auto capped = WorkspaceShellTestAccess::WrapHoverPopupText(shell, text, 120.0f, 2);
  Expect(capped.size() <= 2, "the line cap must be honored");
  Expect(capped != narrow, "changing the line cap must not return the uncapped cache entry");

  // Different text with identical width/cap must not collide.
  const auto other = WorkspaceShellTestAccess::WrapHoverPopupText(
      shell, "a completely different hover string entirely", 120.0f, 8);
  Expect(other != narrow, "different text must not return another entry's lines");

  // The memo holds a small fixed number of entries; cycling past it must not
  // corrupt a later lookup of an evicted key.
  for (int i = 1; i <= 12; ++i) {
    WorkspaceShellTestAccess::WrapHoverPopupText(shell, text, 120.0f + static_cast<float>(i), 8);
  }
  Expect(WorkspaceShellTestAccess::WrapHoverPopupText(shell, text, 120.0f, 8) == narrow,
         "an evicted key must be recomputed to the same lines, not served from a wrong slot");

  // The memo's fourth key component — the text renderer's metrics generation — is
  // deliberately NOT asserted here: these tests run with no text backend, where
  // MeasureWidth is a fixed 8px per byte and SetFontPointSize is a no-op that does
  // not even bump the generation. Nothing observable would change, so an assertion
  // would pass whether or not the term were in the key. The term guards a real case
  // (a font size / family / presentation-scale change re-wraps at new advances);
  // its three invalidation points are ClearWidthCache's call sites in TextRenderer.
}

}  // namespace

void RegisterWorkspaceShellEditorBlameTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/HoverWrapMemoKeysOnEveryInput",
          TestWorkspaceShellHoverWrapMemoKeysOnEveryInput);
  AddTest(tests, "WorkspaceShell/EditorBlameLoadsForCleanTrackedFile",
          TestWorkspaceShellEditorBlameLoadsForCleanTrackedFile);
  AddTest(tests, "WorkspaceShell/EditorBlameLoadsForLargeTrackedFile",
          TestWorkspaceShellEditorBlameLoadsForLargeTrackedFile);
  AddTest(tests, "WorkspaceShell/EditorBlameTracksStickyScrollYOffset",
          TestWorkspaceShellEditorBlameTracksStickyScrollYOffset);
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
