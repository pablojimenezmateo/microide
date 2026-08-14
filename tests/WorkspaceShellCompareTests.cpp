#include "TestSupport.h"

#include "support/SoftwareCanvas.h"

#include "workspace/render/DiffDividerGeometry.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"
#include "render/Theme.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include "WorkspaceShellEventHelpers.h"

namespace microide::tests {
namespace {


using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

// Divider grab rects for the active compare / merge tab. DiffDividerGeometry.h
// takes WorkspaceShell by name, so it cannot be reached from inside the TestAccess
// class body; these live here instead and read the same geometry production does.
SDL_FRect CompareDividerRectOf(microide::workspace::WorkspaceShell& shell) {
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  return microide::workspace::CompareDividerHitRect(
      layout.editor_surface, WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell));
}

std::array<SDL_FRect, 2> MergeDividerRectsOf(microide::workspace::WorkspaceShell& shell) {
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  return microide::workspace::MergeDividerHitRects(
      layout.editor_surface, WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell));
}

#if MICROIDE_HAS_SDL3_TTF

void EnsureDummySdlVideo() {
  static ScopedEnvVar video_driver("SDL_VIDEODRIVER", "dummy");
  static const bool initialized = SDL_Init(SDL_INIT_VIDEO);
  Expect(initialized, "SDL should initialize with the dummy video driver");
}


#endif

// The compare/ref picker now runs its git query on the background executor and
// populates on a later frame via the git-sidebar wake event. Drive the mailbox
// drain until the picker leaves its loading state (or a 2s deadline elapses).
bool SettleComparePicker(WorkspaceShell& shell) {
  return WaitUntil(
      [&shell]() { return !WorkspaceShellTestAccess::ComparePickerLoading(shell); },
      std::chrono::seconds(2), std::chrono::milliseconds(5),
      [&shell]() { WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell); });
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

// TD-2026-08-13-207: the compare pane's key handling was a hand-maintained subset
// of the editor's, so it silently lacked whatever nobody remembered to copy —
// here, Shift+Tab outdent and Tab-indents-a-multi-line-selection. Both surfaces
// now run one shared switch, so a key added to it reaches both.
void TestWorkspaceShellCompareEditablePaneIndentsAndOutdents() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare indent fixture", "compare indent fixture");
  WriteFile(source, "alpha\nbravo\ncharlie\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.right_editable && compare.right_view_active,
         "the compare fixture should start on the editable pane");

  // Select the first two lines, then Tab. A multi-line selection indents as a
  // block; the old compare switch replaced the selection with a tab character.
  compare.right_viewport.MoveCursorTo(0, 0);
  compare.right_viewport.MoveCursorTo(1, 5, /*extend_selection=*/true);
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE),
         "Tab should be handled on the compare editable pane");
  Expect(compare.right_viewport.lines().LineView(0).rfind("  alpha", 0) == 0 ||
             compare.right_viewport.lines().LineView(0).rfind("\talpha", 0) == 0,
         "Tab with a multi-line selection indents the block instead of replacing it");
  Expect(compare.right_viewport.lines().LineView(1).find("bravo") != std::string::npos,
         "the second selected line survives the block indent");

  // Shift+Tab outdents it again.
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_SHIFT),
         "Shift+Tab should be handled on the compare editable pane");
  Expect(compare.right_viewport.lines().LineView(0) == "alpha",
         "Shift+Tab outdents the block back rather than inserting a tab");
}

// TD-2026-08-13-206's last render feature: bracket-match highlight on the
// editable pane. FindBracketMatch is O(file), so the entry left it out rather
// than put an uncached scan on a per-frame path. The memo is what makes it
// affordable, so the memo is what this pins: a repaint with an unchanged caret
// and unchanged content must not rescan, and moving the caret must.
void TestWorkspaceShellCompareBracketMatchIsMemoized() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add bracket fixture", "bracket fixture");
  WriteFile(source, "int beta(int x) {\n  return (x + 1);\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.right_view_active, "the editable pane is active");
  Expect(!compare.bracket_match_valid, "the memo starts cold");

  // A REAL renderer: RenderFrame's null one stops at the `renderer == nullptr`
  // guard every surface opens with, so it would prove nothing about paint state.
  SoftwareCanvas canvas(1280, 720);

  // Put the caret next to the '(' on the return line.
  compare.right_viewport.MoveCursorTo(1, 9);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  Expect(compare.bracket_match_valid, "painting resolves and memoizes the bracket match");
  const auto first_pair = compare.bracket_match_pair;
  const std::uint64_t revision_after_first = compare.bracket_match_content_revision;

  // A repaint with nothing changed must reuse the memo: same key, same answer.
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  Expect(compare.bracket_match_content_revision == revision_after_first &&
             compare.bracket_match_caret_line == 1 && compare.bracket_match_caret_column == 9,
         "an unchanged repaint keeps the memo key");
  Expect(compare.bracket_match_pair.has_value() == first_pair.has_value(),
         "an unchanged repaint keeps the memoized answer");

  // Moving the caret changes the key, so the next paint re-resolves.
  compare.right_viewport.MoveCursorTo(0, 0);
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  Expect(compare.bracket_match_caret_line == 0 && compare.bracket_match_caret_column == 0,
         "moving the caret re-keys the memo rather than serving the old match");
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
  Expect(SendMouseDown(shell, left_x, row_y, SDL_BUTTON_LEFT),
         "clicking the compare left pane should be handled");
  Expect(!compare.right_view_active,
         "clicking the compare left pane should leave the editable pane inactive");

  Expect(SendMouseDown(shell, right_x, row_y, SDL_BUTTON_LEFT),
         "clicking the compare right pane should be handled");
  Expect(compare.right_view_active,
         "clicking the compare right pane should reactivate the editable pane");
}

void TestWorkspaceShellCompareClickAboveFirstRowIsNotHandled() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare band fixture", "compare band fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare band fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float left_x = surface.left_x + 24.0f;

  // Select a non-zero presentation row so a spurious "row 0" hit would be observable.
  const float row2_y = surface.rows_y + surface.line_height * 2.5f;
  Expect(SendMouseDown(shell, left_x, row2_y, SDL_BUTTON_LEFT),
         "clicking a real compare row should be handled");
  const std::size_t selected_after_real_click = compare.selected_row;

  // A click in the band directly above the first row (negative row offset) must be
  // rejected, not truncated to a phantom hit on row 0.
  const float above_y = surface.rows_y - 2.0f;
  Expect(!SendMouseDown(shell, left_x, above_y, SDL_BUTTON_LEFT),
         "clicking just above the first compare row must not be handled as a row hit");
  Expect(compare.selected_row == selected_after_real_click,
         "a click above the first row must not move the compare selection to row 0");
}

void TestWorkspaceShellCompareCollapsedContextButtonsExpandHiddenRows() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  auto build_text = [](std::string_view first_change, std::string_view second_change) {
    std::string text;
    for (int i = 0; i < 24; ++i) {
      text += "prefix " + std::to_string(i) + "\n";
    }
    text += std::string(first_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle " + std::to_string(i) + "\n";
    }
    text += std::string(second_change) + "\n";
    for (int i = 0; i < 8; ++i) {
      text += "suffix " + std::to_string(i) + "\n";
    }
    return text;
  };
  WriteFile(source, build_text("left a", "left b"));

  InitializeGitRepo(root);
  CommitAll(root, "Add compare context fixture", "compare context fixture");
  WriteFile(source, build_text("right a", "right b"));

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare collapsed-context fixture should open");

  std::optional<std::size_t> collapsed_row;
  std::pair<std::size_t, std::size_t> collapsed_run_identity{0, 0};
  const std::size_t initial_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  for (std::size_t i = 0; i < initial_row_count; ++i) {
    if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(shell, i) !=
        microide::compare::ComparePresentationRowKind::CollapsedContext) {
      continue;
    }
    const auto action_rects =
        WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, i);
    if (action_rects.previous_rect.has_value() && action_rects.next_rect.has_value()) {
      collapsed_row = i;
      collapsed_run_identity =
          WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(shell, i);
      break;
    }
  }
  Expect(collapsed_row.has_value(),
         "compare collapsed-context fixture should expose a middle hidden context row");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.scroll_row = std::max(0, static_cast<int>(*collapsed_row) - 2);
  const auto action_rects =
      WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, *collapsed_row);
  Expect(action_rects.previous_rect.has_value(),
         "middle collapsed context row should expose a previous-context expansion button");
  Expect(ExpandCompareCollapsedContext(compare, *collapsed_row,
                                       microide::workspace::CompareCollapsedContextAction::ShowAll),
         "expanding collapsed context from the selected run should succeed");
  Expect(WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell) > initial_row_count,
         "expanding collapsed context should reveal additional rows");
  std::optional<std::size_t> updated_collapsed_row;
  const std::size_t updated_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  for (std::size_t i = 0; i < updated_row_count; ++i) {
    if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(shell, i) !=
        microide::compare::ComparePresentationRowKind::CollapsedContext) {
      continue;
    }
    if (WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(shell, i) ==
        collapsed_run_identity) {
      updated_collapsed_row = i;
      break;
    }
  }
  Expect(!updated_collapsed_row.has_value(),
         "Show all should fully expand the selected collapsed run");
}

void TestWorkspaceShellCompareCollapsedContextButtonsHoverAsInteractive() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const auto build_text = [](std::string_view first_change, std::string_view second_change) {
    std::string text;
    for (int i = 0; i < 24; ++i) {
      text += "prefix " + std::to_string(i) + "\n";
    }
    text += std::string(first_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle " + std::to_string(i) + "\n";
    }
    text += std::string(second_change) + "\n";
    for (int i = 0; i < 8; ++i) {
      text += "suffix " + std::to_string(i) + "\n";
    }
    return text;
  };
  WriteFile(source, build_text("left a", "left b"));

  InitializeGitRepo(root);
  CommitAll(root, "Add compare hover fixture", "compare hover fixture");
  WriteFile(source, build_text("right a", "right b"));

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare collapsed-context hover fixture should open");

  std::optional<std::size_t> collapsed_row;
  const std::size_t row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  for (std::size_t i = 0; i < row_count; ++i) {
    if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(shell, i) !=
        microide::compare::ComparePresentationRowKind::CollapsedContext) {
      continue;
    }
    const auto action_rects =
        WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, i);
    if (action_rects.previous_rect.has_value() && action_rects.next_rect.has_value()) {
      collapsed_row = i;
      break;
    }
  }
  Expect(collapsed_row.has_value(),
         "compare collapsed-context hover fixture should expose a middle hidden context row");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.scroll_row = std::max(0, static_cast<int>(*collapsed_row) - 2);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const auto action_rects =
      WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, *collapsed_row);
  const float hover_x = action_rects.all_rect.x + action_rects.all_rect.w * 0.5f;
  const float hover_y = action_rects.all_rect.y + action_rects.all_rect.h * 0.5f;
  Expect(WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, hover_x, hover_y),
         "collapsed-context action buttons should advertise a pointer cursor");
  SendMouseMotion(shell, hover_x, hover_y, 0);
  Expect(WorkspaceShellTestAccess::CachedCursorIsPointer(shell),
         "hovering a collapsed-context action button should cache the pointer cursor");

  // The cursor shape, the hover highlight and the click each used to re-derive
  // "which collapsed-context action is under this point" from scratch. They share
  // CompareCollapsedContextRowAt + CompareCollapsedContextActionAt now; assert the
  // two observable halves agree on every button, so a future divergence shows up
  // as a hand cursor over something that does not highlight.
  //
  // Rects are re-read per button: a motion event re-clamps compare scroll, which
  // moves the row under the pointer.
  const microide::workspace::CompareHoverKind kinds[] = {
      microide::workspace::CompareHoverKind::CollapsedContextPreviousAction,
      microide::workspace::CompareHoverKind::CollapsedContextAllAction,
      microide::workspace::CompareHoverKind::CollapsedContextNextAction,
  };
  for (const microide::workspace::CompareHoverKind expected_kind : kinds) {
    const auto rects =
        WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, *collapsed_row);
    const std::optional<SDL_FRect> rect =
        expected_kind == microide::workspace::CompareHoverKind::CollapsedContextPreviousAction
            ? rects.previous_rect
        : expected_kind == microide::workspace::CompareHoverKind::CollapsedContextNextAction
            ? rects.next_rect
            : std::optional<SDL_FRect>(rects.all_rect);
    Expect(rect.has_value() && rect->w > 0.0f,
           "the fixture row should expose all three action buttons");
    const float by = rect->y + rect->h * 0.5f;
    // Probe the edges, not just the centre: the buttons are ~140px wide, so a
    // centre probe survives the whole button shifting by the scrollbar reserve
    // plus the block inset — which is exactly how the two paths drift apart.
    for (const float bx : {rect->x + 1.0f, rect->x + rect->w * 0.5f, rect->x + rect->w - 1.0f}) {
      Expect(WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, bx, by),
             "every collapsed-context action button should advertise a pointer cursor");
      SendMouseMotion(shell, bx, by, 0);
      Expect(WorkspaceShellTestAccess::ActiveCompareHoverKind(shell) == expected_kind,
             "the hover highlight should resolve the same button the cursor points at");
    }
  }

  // Inside the block but off every button: neither half may claim a hit.
  {
    const auto rects =
        WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, *collapsed_row);
    const float gap_x = rects.all_rect.x - 6.0f;
    const float gap_y = rects.all_rect.y + rects.all_rect.h * 0.5f;
    if (gap_x > surface.left_x && (!rects.previous_rect.has_value() ||
                                   gap_x > rects.previous_rect->x + rects.previous_rect->w)) {
      SendMouseMotion(shell, gap_x, gap_y, 0);
      Expect(!WorkspaceShellTestAccess::ActiveCompareHoverKind(shell).has_value(),
             "the gap between action buttons should not highlight one");
      Expect(!WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, gap_x, gap_y),
             "the cursor may only offer a hand where the hover highlight agrees");
    }
  }

  const float clear_x = surface.right_x + 12.0f;
  SendMouseMotion(shell, clear_x, hover_y, 0);
  Expect(WorkspaceShellTestAccess::CachedCursorIsText(shell),
         "ordinary editable compare content should restore the text cursor");
}

void TestWorkspaceShellCompareCollapsedContextExpansionPersistsAcrossChunks() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const auto build_text = [](std::string_view first_change,
                             std::string_view second_change,
                             std::string_view third_change) {
    std::string text;
    for (int i = 0; i < 24; ++i) {
      text += "prefix " + std::to_string(i) + "\n";
    }
    text += std::string(first_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle-a " + std::to_string(i) + "\n";
    }
    text += std::string(second_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle-b " + std::to_string(i) + "\n";
    }
    text += std::string(third_change) + "\n";
    for (int i = 0; i < 8; ++i) {
      text += "suffix " + std::to_string(i) + "\n";
    }
    return text;
  };
  WriteFile(source, build_text("left a", "left b", "left c"));

  InitializeGitRepo(root);
  CommitAll(root, "Add compare multi-chunk fixture", "compare multi-chunk fixture");
  WriteFile(source, build_text("right a", "right b", "right c"));

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare multi-chunk fixture should open");

  const auto collect_middle_runs = [&](WorkspaceShell& current_shell) {
    std::vector<std::pair<std::size_t, std::size_t>> identities;
    const std::size_t row_count =
        WorkspaceShellTestAccess::ActiveComparePresentationRowCount(current_shell);
    for (std::size_t i = 0; i < row_count; ++i) {
      if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(current_shell, i) !=
          microide::compare::ComparePresentationRowKind::CollapsedContext) {
        continue;
      }
      const auto action_rects =
          WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(current_shell, i);
      if (action_rects.previous_rect.has_value() && action_rects.next_rect.has_value()) {
        identities.push_back(
            WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(current_shell, i));
      }
    }
    return identities;
  };
  const auto find_row = [&](WorkspaceShell& current_shell,
                            std::pair<std::size_t, std::size_t> identity) {
    const std::size_t row_count =
        WorkspaceShellTestAccess::ActiveComparePresentationRowCount(current_shell);
    for (std::size_t i = 0; i < row_count; ++i) {
      if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(current_shell, i) !=
          microide::compare::ComparePresentationRowKind::CollapsedContext) {
        continue;
      }
      if (WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(current_shell, i) ==
          identity) {
        return std::optional<std::size_t>(i);
      }
    }
    return std::optional<std::size_t>{};
  };
  const std::vector<std::pair<std::size_t, std::size_t>> middle_runs = collect_middle_runs(shell);
  Expect(middle_runs.size() >= 2,
         "compare multi-chunk fixture should expose at least two independently collapsed middle runs");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const std::optional<std::size_t> first_row = find_row(shell, middle_runs[0]);
  Expect(first_row.has_value(), "first collapsed middle run should be present before expansion");
  Expect(ExpandCompareCollapsedContext(compare, *first_row,
                                       microide::workspace::CompareCollapsedContextAction::ShowAll),
         "ShowAll should expand the first collapsed middle run");
  const std::size_t after_first_expand =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  Expect(!find_row(shell, middle_runs[0]).has_value(),
         "the first collapsed middle run should remain expanded");

  const std::optional<std::size_t> second_row = find_row(shell, middle_runs[1]);
  Expect(second_row.has_value(),
         "the second collapsed middle run should remain available after the first expansion");
  Expect(ExpandCompareCollapsedContext(compare, *second_row,
                                       microide::workspace::CompareCollapsedContextAction::ShowAll),
         "ShowAll should expand the second collapsed middle run");
  Expect(WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell) > after_first_expand,
         "expanding a second collapsed middle run should reveal additional rows");
  Expect(!find_row(shell, middle_runs[0]).has_value(),
         "expanding a second run should not collapse the first one again");
  Expect(!find_row(shell, middle_runs[1]).has_value(),
         "the second collapsed middle run should also remain expanded");
}

void TestWorkspaceShellCompareCollapsedContextExpansionSurvivesTreeRefresh() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const auto build_text = [](std::string_view first_change, std::string_view second_change) {
    std::string text;
    for (int i = 0; i < 24; ++i) {
      text += "prefix " + std::to_string(i) + "\n";
    }
    text += std::string(first_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle " + std::to_string(i) + "\n";
    }
    text += std::string(second_change) + "\n";
    for (int i = 0; i < 8; ++i) {
      text += "suffix " + std::to_string(i) + "\n";
    }
    return text;
  };
  const std::string left = build_text("left a", "left b");
  const std::string right = build_text("right a", "right b");
  WriteFile(source, left);

  InitializeGitRepo(root);
  CommitAll(root, "Add compare refresh fixture", "compare refresh fixture");
  WriteFile(source, right);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare refresh fixture should open");

  std::optional<std::size_t> collapsed_row;
  std::pair<std::size_t, std::size_t> collapsed_run_identity{0, 0};
  const std::size_t initial_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  for (std::size_t i = 0; i < initial_row_count; ++i) {
    if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(shell, i) !=
        microide::compare::ComparePresentationRowKind::CollapsedContext) {
      continue;
    }
    const auto action_rects =
        WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, i);
    if (action_rects.previous_rect.has_value() && action_rects.next_rect.has_value()) {
      collapsed_row = i;
      collapsed_run_identity =
          WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(shell, i);
      break;
    }
  }
  Expect(collapsed_row.has_value(),
         "compare refresh fixture should expose a collapsed middle run before expansion");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.scroll_row = std::max(0, static_cast<int>(*collapsed_row) - 2);
  Expect(ExpandCompareCollapsedContext(compare, *collapsed_row,
                                       microide::workspace::CompareCollapsedContextAction::ShowAll),
         "ShowAll should expand the collapsed middle run before refresh");
  const std::size_t expanded_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  Expect(expanded_row_count > initial_row_count,
         "expanding the middle collapsed run should reveal additional rows");

  Expect(WorkspaceShellTestAccess::ExecuteTreeRefresh(shell),
         "tree refresh should execute for compare refresh fixture");
  Expect(WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell) == expanded_row_count,
         "tree refresh should preserve the expanded compare presentation row count");

  bool run_recollapsed = false;
  const std::size_t refreshed_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  for (std::size_t i = 0; i < refreshed_row_count; ++i) {
    if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(shell, i) !=
        microide::compare::ComparePresentationRowKind::CollapsedContext) {
      continue;
    }
    if (WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(shell, i) ==
        collapsed_run_identity) {
      run_recollapsed = true;
      break;
    }
  }
  Expect(!run_recollapsed,
         "tree refresh should preserve expanded compare runs instead of re-collapsing them");
}

void TestWorkspaceShellReadOnlyCompareRightPaneSupportsSelectAllAndCopy() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "history.txt";
  WriteFile(source, "base line\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "head line\n");
  CommitAll(root, "head fixture", "head fixture");

  const auto history = microide::project::CollectGitFileHistory(root, source).commits;
  Expect(history.size() == 2, "read-only compare fixture should have two commits");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenBranchHeadComparison(shell, source, history[1].hash, "base",
                                                            history[0].hash, "head"),
         "read-only branch comparison should open");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float row_y = surface.rows_y + surface.line_height * 0.5f;
  const float right_x = surface.right_x + 24.0f;
  Expect(SendMouseDown(shell, right_x, row_y, SDL_BUTTON_LEFT),
         "clicking the read-only compare right pane should be handled");
  Expect(WorkspaceShellTestAccess::ActiveCompare(shell).right_view_active,
         "clicking the read-only compare right pane should make it the active navigable surface");

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  Expect(WorkspaceShellTestAccess::ExecuteSelectAll(shell),
         "Select All should execute on the read-only compare right pane");
  Expect(WorkspaceShellTestAccess::ExecuteCopySelection(shell),
         "Copy Selection should execute on the read-only compare right pane");
  Expect(clipboard_text.find("head line") != std::string::npos,
         "copying from the read-only compare right pane should copy its visible text");
}

void TestWorkspaceShellReadOnlyCompareShortcutCopyUsesNavigableViewport() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "history.txt";
  WriteFile(source, "base line\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "head line\n");
  CommitAll(root, "head fixture", "head fixture");

  const auto history = microide::project::CollectGitFileHistory(root, source).commits;
  Expect(history.size() == 2, "compare shortcut fixture should have two commits");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenBranchHeadComparison(shell, source, history[1].hash, "base",
                                                            history[0].hash, "head"),
         "read-only branch comparison should open");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float row_y = surface.rows_y + surface.line_height * 0.5f;
  const float right_x = surface.right_x + 24.0f;
  Expect(SendMouseDown(shell, right_x, row_y, SDL_BUTTON_LEFT),
         "clicking the read-only compare right pane should be handled");

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  Expect(SendKeyDown(shell, SDLK_A, SDL_KMOD_CTRL),
         "Ctrl+A should be handled on the read-only compare pane");
  Expect(SendKeyDown(shell, SDLK_C, SDL_KMOD_CTRL),
         "Ctrl+C should be handled on the read-only compare pane");
  Expect(clipboard_text.find("head line") != std::string::npos,
         "compare shortcut copy should use the active read-only navigable viewport");
}

void TestWorkspaceShellCompareWheelScrollsRows() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";

  std::string base_text;
  std::string working_text;
  for (int i = 0; i < 120; ++i) {
    base_text += "base line " + std::to_string(i) + "\n";
    working_text += ((i % 5 == 0) ? "changed line " + std::to_string(i) + "\n"
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
  Expect(workspace::CompareTabPresentationRowCount(compare) >
             static_cast<std::size_t>(surface.visible_rows),
         "compare wheel fixture should overflow the viewport");

  const int before_scroll = compare.scroll_row;
  const float wheel_x = surface.right_x + 24.0f;
  const float wheel_y = surface.rows_y + surface.line_height * 0.5f;
  // The wheel must not pull keyboard focus out of whatever the user is typing into,
  // which is what every other scrollable surface already promises. Compare and merge
  // were the two that claimed focus for the editor on every tick, so a wheel nudge
  // over the diff silently redirected the next keystroke away from the terminal.
  WorkspaceShellTestAccess::SetFocusPanel(shell);
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, -1),
         "scrolling the compare surface should be handled");
  Expect(compare.scroll_row > before_scroll,
         "scrolling the compare surface should advance the visible row");
  Expect(WorkspaceShellTestAccess::FocusIsPanel(shell),
         "scrolling the compare surface should leave keyboard focus where it was");
}

void TestWorkspaceShellCompareWheelScrollsColumns() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";

  const std::string base_line =
      "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789\n";
  const std::string working_line =
      "abcdefghijklmnopqrstuvwxyz9876543210abcdefghijklmnopqrstuvwxyz9876543210\n";
  WriteFile(source, base_line);

  InitializeGitRepo(root);
  CommitAll(root, "Add compare horizontal wheel fixture", "compare horizontal wheel fixture");
  WriteFile(source, working_line);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 640, 420);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare horizontal wheel fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(compare.max_visual_columns > surface.visible_columns,
         "compare horizontal wheel fixture should overflow horizontally");

  const std::size_t before_scroll = compare.horizontal_scroll;
  const float wheel_x = surface.right_x + 24.0f;
  const float wheel_y = surface.rows_y + surface.line_height * 0.5f;
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, 0, -1),
         "horizontal scrolling the compare surface should be handled");
  Expect(compare.horizontal_scroll > before_scroll,
         "horizontal scrolling the compare surface should advance the visible column");
}

void TestWorkspaceShellCompareWheelScrollsLeftDominatedColumns() {
  // Regression: the two panes share one horizontal offset, but it used to be
  // round-tripped through the editable right viewport, which clamps to the right
  // document's longest line. When the overflowing content lives only on the left
  // (a long line shortened/deleted in the working tree), that read-back forced the
  // shared offset back to 0 and horizontal scrolling appeared completely dead.
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";

  // Long line at HEAD (left pane), collapsed to a short line in the working tree
  // (right pane). The right document on its own never overflows the viewport.
  const std::string base_line =
      "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789\n";
  const std::string working_line = "short\n";
  WriteFile(source, base_line);

  InitializeGitRepo(root);
  CommitAll(root, "Add compare left-dominated fixture", "compare left-dominated fixture");
  WriteFile(source, working_line);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 640, 420);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare left-dominated fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(compare.max_visual_columns > surface.visible_columns,
         "compare left-dominated fixture should overflow horizontally via the left pane");

  const std::size_t before_scroll = compare.horizontal_scroll;
  const float wheel_x = surface.right_x + 24.0f;
  const float wheel_y = surface.rows_y + surface.line_height * 0.5f;
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, 0, -1),
         "horizontal scrolling a left-dominated compare should be handled");
  Expect(compare.horizontal_scroll > before_scroll,
         "left-pane overflow should still advance the shared horizontal offset");
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
  (void)shell.ConsumePendingRenderInvalidation();

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
  const auto redraw_rect = result.redraw.SingleRectIfOnlyOne();

  Expect(result.handled, "compare horizontal navigation should be handled");
  Expect(!result.redraw.full && redraw_rect.has_value(),
         "compare horizontal navigation should stay on a partial redraw path");
  Expect(RectsIntersect(*redraw_rect, editable_rect),
         "compare horizontal navigation should repaint the editable pane");
  Expect(!RectsIntersect(*redraw_rect, left_rect),
         "compare horizontal navigation should avoid repainting the historical left pane");
}

void TestWorkspaceShellCompareSelectionStepInvalidatesRowBand() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "one\ntwo\nthree\nfour\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare row invalidation fixture", "compare row invalidation fixture");
  WriteFile(source, "one\nTWO\nthree\nfour\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare row invalidation fixture should open");
  (void)shell.ConsumePendingRenderInvalidation();

  const auto previous_row_rect = WorkspaceShellTestAccess::ActiveCompareRowRangeRect(shell, 0, 1);
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = SDLK_DOWN;
  const auto result = shell.HandleEvent(event);
  const auto next_row_rect = WorkspaceShellTestAccess::ActiveCompareRowRangeRect(shell, 1, 2);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);

  Expect(result.handled, "compare selection step should be handled");
  Expect(!result.redraw.full && !result.redraw.rects.empty(),
         "compare selection step should stay on a partial redraw path");
  Expect(previous_row_rect.has_value() && AnyRectCovers(result.redraw.rects, *previous_row_rect),
         "compare selection step should repaint the previously selected row");
  Expect(next_row_rect.has_value() && AnyRectCovers(result.redraw.rects, *next_row_rect),
         "compare selection step should repaint the newly selected row");
  Expect(MaxRectHeight(result.redraw.rects) <
             static_cast<float>(surface.visible_rows) * surface.line_height,
         "compare selection step should redraw less than the full compare surface height");
}

void TestWorkspaceShellCompareRenderPaintsDiagnosticGutterMarkers() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare diagnostics fixture", "compare diagnostics fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare diagnostics fixture should open");
  Expect(WorkspaceShellTestAccess::PublishDiagnostics(
             shell, "diagnostics", source,
             {microide::editor::Diagnostic{
                 .range =
                     microide::editor::SelectionRange{
                         .start = microide::editor::TextPosition{.line = 0, .column = 0},
                         .end = microide::editor::TextPosition{.line = 0, .column = 3},
                     },
                 .severity = microide::editor::DiagnosticSeverity::Error,
                 .message = "compare error",
             }}),
         "compare diagnostics fixture should publish one right-pane diagnostic");

  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "compare diagnostics render test should read software pixels");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const auto theme = microide::render::MakeDefaultTheme();
  Uint8 r = 0;
  Uint8 g = 0;
  Uint8 b = 0;
  Uint8 a = 0;
  const int marker_x = static_cast<int>(std::floor(surface.right_x + 3.0f));
  const int marker_y = static_cast<int>(std::floor(surface.rows_y + surface.line_height - 2.0f));
  Expect(SDL_ReadSurfacePixel(pixels, marker_x, marker_y, &r, &g, &b, &a),
         "compare diagnostics render test should read the gutter marker pixel");
  Expect(r == theme.diagnostic_error.r && g == theme.diagnostic_error.g &&
             b == theme.diagnostic_error.b && a == theme.diagnostic_error.a,
         "compare right-pane diagnostics should paint severity markers in the gutter");

  SDL_DestroySurface(pixels);
}

void TestWorkspaceShellCompareRenderReusesVisibleLayoutCache() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "same\tline\nold\tline\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare layout cache fixture", "compare layout cache fixture");
  WriteFile(source, "same\tline\nnew\tline\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare layout cache fixture should open");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.visible_layout_cache.empty(),
         "compare visible-layout cache should start empty before first render");

  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  const std::size_t warmed_cache_size = compare.visible_layout_cache.size();
  Expect(warmed_cache_size >= 2,
         "first compare render should warm visible layouts for rendered panes");

  shell.Render(canvas.renderer(), 1280, 720);
  Expect(compare.visible_layout_cache.size() == warmed_cache_size,
         "stable compare frames should reuse cached visible layouts instead of appending more");

  compare.left_content = "same\tline\nolder\tline\n";
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.visible_layout_cache.empty(),
         "compare visible-layout cache should clear when the compare model changes");
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

  const auto overlay = WaitForActiveCompareBlameOverlay(shell, 1);
  Expect(overlay.has_value(),
         "clean working-tree comparison should eventually expose compare blame");
  Expect(overlay->lines.size() == 1,
         "compare blame should annotate the caret line only, like the editor surface");
  Expect(overlay->lines[0].author == "Microide Tests",
         "compare blame should keep the blame author metadata");
  Expect(overlay->lines[0].summary == "Add compare blame fixture",
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

  const auto overlay = WaitForActiveMergeBlameOverlay(shell, 1);
  Expect(overlay.has_value(),
         "clean merge result pane should eventually expose blame");
  Expect(overlay->lines.size() == 1,
         "merge blame should annotate the caret line only, like the editor surface");
  Expect(overlay->lines[0].author == "Microide Tests",
         "merge blame should keep the blame author metadata");
  Expect(overlay->lines[0].summary == "Add merge blame fixture",
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

// A conflicted binary/NUL worktree file must NOT build a text merge tab (the compare
// path already refuses these). Building one would let the result viewport later save text
// over binary bytes (TD-2026-07-17A-111). BuildMergeTabEntry classifies the three inputs
// and OpenMergeEditor returns false when any is binary/too-large/unreadable.
void TestWorkspaceShellMergeEditorRefusesBinaryInput() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.bin";
  const std::filesystem::path incoming = root / "incoming.bin";
  const std::filesystem::path current = root / "current.bin";
  const std::filesystem::path output = root / "result.bin";
  WriteFile(base, "top\nbase\nbottom\n");
  WriteFile(incoming, "top\nincoming\nbottom\n");
  // Current side carries an embedded NUL — classified as binary, not editable text.
  WriteFile(current, std::string("top\ncur\0rent\nbottom\n", 20));
  WriteFile(output, "top\ncurrent\nbottom\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(!WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "a binary/NUL merge input must not build a text merge tab");

  // Sanity: with the same paths made all-text, the merge editor opens.
  WriteFile(current, "top\ncurrent\nbottom\n");
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "an all-text merge input should still open the merge editor");
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

void TestWorkspaceShellCompareRenderKeepsDividerBorderOnUnchangedRows() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "same line\nold line\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare divider border fixture", "compare divider border fixture");
  WriteFile(source, "same line\nnew line\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare divider border fixture should open");

  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "compare divider border render test should read software pixels");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const auto theme = microide::render::MakeDefaultTheme();
  Uint8 r = 0;
  Uint8 g = 0;
  Uint8 b = 0;
  Uint8 a = 0;
  const int border_x = static_cast<int>(std::floor(surface.center_x));
  const int border_y = static_cast<int>(std::floor(surface.rows_y + surface.line_height * 0.5f));
  Expect(SDL_ReadSurfacePixel(pixels, border_x, border_y, &r, &g, &b, &a),
         "compare divider border render test should read the divider border pixel");
  Expect(r == theme.border.r && g == theme.border.g &&
             b == theme.border.b && a == theme.border.a,
         "compare divider should keep a visible left border on unchanged rows");

  SDL_DestroySurface(pixels);
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

// "Every resize divider answers a double-click by restoring its default size" was
// implemented for four of the six: the sidebar, right pane, editor split and bottom
// panel. Compare and merge — the two whose panes you are most likely to have dragged
// far off balance while reading a diff — had no way back but dragging by hand.
void TestWorkspaceShellCompareAndMergeDividersResetOnDoubleClick() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path compare_source = root / "src" / "compare.cpp";
  const std::filesystem::path base = root / "src" / "base.cpp";
  const std::filesystem::path incoming = root / "src" / "incoming.cpp";
  const std::filesystem::path current = root / "src" / "current.cpp";
  const std::filesystem::path output = root / "src" / "output.cpp";

  WriteFile(compare_source, "abcdefghijklmnopqrstuvwxyz0123456789\n");
  WriteFile(base, "int answer() {\n  return 0;\n}\n");
  WriteFile(incoming, "int answer() {\n  return 1;\n}\n");
  WriteFile(current, "int answer() {\n  return 2;\n}\n");
  WriteFile(output, "int answer() {\n  return 0;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add divider reset fixture", "divider reset fixture");
  WriteFile(compare_source, "abcdefghijklmnopqrstuvwxyz9876543210\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, compare_source, "HEAD", "HEAD"),
         "divider reset compare fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.divider_fraction = 0.8f;
  const SDL_FRect compare_divider = CompareDividerRectOf(shell);
  Expect(SendMouseDown(shell, compare_divider.x + compare_divider.w * 0.5f,
                       compare_divider.y + compare_divider.h * 0.5f, SDL_BUTTON_LEFT, 2),
         "double-clicking the compare divider should be handled");
  Expect(std::abs(compare.divider_fraction -
                  microide::workspace::kWorkspaceDefaultCompareDividerFraction) < 0.001f,
         "double-clicking the compare divider should restore the even split");
  Expect(WorkspaceShellTestAccess::TransientDragTargetIsNone(shell),
         "a divider reset must not leave a drag armed behind it");

  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "divider reset merge fixture should open");
  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  merge.left_divider_fraction = 0.15f;
  merge.right_divider_fraction = 0.85f;
  const auto merge_dividers = MergeDividerRectsOf(shell);
  // Either divider resets both, so one gesture recovers the whole layout.
  Expect(SendMouseDown(shell, merge_dividers[1].x + merge_dividers[1].w * 0.5f,
                       merge_dividers[1].y + merge_dividers[1].h * 0.5f, SDL_BUTTON_LEFT, 2),
         "double-clicking the merge right divider should be handled");
  Expect(std::abs(merge.left_divider_fraction -
                  microide::workspace::kWorkspaceDefaultMergeLeftDividerFraction) < 0.001f &&
             std::abs(merge.right_divider_fraction -
                      microide::workspace::kWorkspaceDefaultMergeRightDividerFraction) < 0.001f,
         "double-clicking a merge divider should restore both to equal thirds");
  Expect(WorkspaceShellTestAccess::TransientDragTargetIsNone(shell),
         "a merge divider reset must not leave a drag armed behind it");

  // Grabbing a merge divider must hold the resize cursor for the whole gesture.
  // Dragging necessarily moves the pointer off the thin divider band, and only the
  // sidebar and bottom-panel drags used to keep their shape through that.
  // Re-read the geometry: the reset moved both dividers.
  const auto reset_dividers = MergeDividerRectsOf(shell);
  Expect(SendMouseDown(shell, reset_dividers[0].x + reset_dividers[0].w * 0.5f,
                       reset_dividers[0].y + reset_dividers[0].h * 0.5f, SDL_BUTTON_LEFT),
         "grabbing the merge left divider should be handled");
  Expect(!WorkspaceShellTestAccess::TransientDragTargetIsNone(shell),
         "a single click on the merge divider should arm the resize drag");
  Expect(WorkspaceShellTestAccess::CursorKindAtIsEwResize(shell, 10.0f, 10.0f),
         "a merge divider drag should hold the resize cursor away from the divider");
  SendMouseUp(shell, reset_dividers[0].x, reset_dividers[0].y, SDL_BUTTON_LEFT);
}

void TestWorkspaceShellCompareAndMergePaneMinimaPreserveVisibleColumns() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path compare_source = root / "src" / "compare.cpp";
  const std::filesystem::path base = root / "src" / "base.cpp";
  const std::filesystem::path incoming = root / "src" / "incoming.cpp";
  const std::filesystem::path current = root / "src" / "current.cpp";
  const std::filesystem::path output = root / "src" / "output.cpp";

  WriteFile(compare_source, "abcdefghijklmnopqrstuvwxyz0123456789\n");
  WriteFile(base, "int answer() {\n  return 0;\n}\n");
  WriteFile(incoming, "int answer() {\n  return 1;\n}\n");
  WriteFile(current, "int answer() {\n  return 2;\n}\n");
  WriteFile(output, "int answer() {\n  return 0;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add pane minima fixture", "pane minima fixture");
  WriteFile(compare_source, "abcdefghijklmnopqrstuvwxyz9876543210\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, compare_source, "HEAD", "HEAD"),
         "pane minima compare fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.divider_fraction = 0.0f;
  const auto compare_left_min = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  compare.divider_fraction = 1.0f;
  const auto compare_right_min = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(compare_left_min.left_visible_columns >= 1 && compare_left_min.right_visible_columns >= 1,
         "compare layout should preserve one visible column per pane at minimum left fraction");
  Expect(compare_right_min.left_visible_columns >= 1 && compare_right_min.right_visible_columns >= 1,
         "compare layout should preserve one visible column per pane at minimum right fraction");

  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "pane minima merge fixture should open");
  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  merge.left_divider_fraction = 0.0f;
  merge.right_divider_fraction = 1.0f;
  const auto merge_surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  Expect(merge_surface.visible_columns >= 1,
         "merge layout should preserve one visible column at minimum divider fractions");
}

void TestWorkspaceShellMergeToolbarLayoutClearsPaneHeaders() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "conflict.txt";
  WriteFile(base, "line 1\nshared\n");
  WriteFile(incoming, "line 1\nincoming change\nline 3\n");
  WriteFile(current, "line 1\ncurrent change\nline 3\n");
  WriteFile(output, "line 1\nshared\nline 3\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1920, 1080);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge toolbar layout fixture should open");

  const auto surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  const auto toolbar = WorkspaceShellTestAccess::MergeToolbarNavigationRects(shell);

  constexpr float kMergeToolbarButtonHeight = 22.0f;
  Expect(surface.header_y >= surface.secondary_button_y + kMergeToolbarButtonHeight + 4.0f,
         "pane headers should sit below the secondary toolbar row");
  Expect(surface.rows_y >= surface.header_y + surface.line_height,
         "merge rows should start below pane headers");
  Expect(toolbar[0].y + toolbar[0].h <= surface.secondary_button_y + 0.5f,
         "primary toolbar should stay on the first row");
  Expect(surface.secondary_button_y + kMergeToolbarButtonHeight <= surface.header_y + 0.5f,
         "secondary toolbar should stay above pane headers");
}

void TestWorkspaceShellCommitPickerDismissesAfterOpeningCompare() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add commit picker fixture", "commit picker fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");
  CommitAll(root, "Edit commit picker fixture", "commit picker fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(WorkspaceShellTestAccess::OpenComparePickerForPath(shell, source),
         "opening the commit picker for a file with history should succeed");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell),
         "the commit picker overlay should be visible after opening");
  Expect(WorkspaceShellTestAccess::ActiveOverlayMode(shell) ==
             WorkspaceShell::OverlayMode::CommitPicker,
         "the open overlay should be the commit picker");

  // The overlay opens immediately in a loading state; wait for the async git
  // history query to populate the picker before selecting a commit.
  Expect(SettleComparePicker(shell), "the async commit picker query should settle");

  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "pressing Enter in the commit picker should be handled");

  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
         "selecting a commit should dismiss the picker instead of leaving it over the comparison");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell),
         "selecting a commit should open and focus the comparison tab");
}

microide::project::GitCommitEntry MakeFakeCommit(int index) {
  microide::project::GitCommitEntry commit;
  const std::string suffix = std::to_string(index);
  commit.hash = "deadbeef" + suffix;
  commit.short_hash = "dead" + suffix;
  commit.subject = "fake commit " + suffix;
  commit.author = "tester";
  commit.relative_date = "just now";
  return commit;
}

void TestWorkspaceShellComparePickerOpensAsyncAndDropsStaleResult() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source_a = root / "src" / "a.cpp";
  const std::filesystem::path source_b = root / "src" / "b.cpp";

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  // Injected provider: blocks on `release` until the test lets it return, and
  // returns `call_count` commits so each completion is distinguishable by size.
  std::atomic<bool> release{false};
  std::atomic<int> call_count{0};
  WorkspaceShellTestAccess::SetComparePickerFileHistoryProvider(
      shell,
      [&release, &call_count](const std::filesystem::path&, const std::filesystem::path&) {
        const int this_call = ++call_count;
        while (!release.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        microide::project::GitFileHistoryResult result;
        for (int i = 0; i < this_call; ++i) {
          result.commits.push_back(MakeFakeCommit(i));
        }
        return result;
      });

  // First open: the overlay must appear immediately in a loading state, BEFORE
  // the (blocked) git query returns. This is the core win of the async change.
  Expect(WorkspaceShellTestAccess::OpenComparePickerForPath(shell, source_a),
         "opening the async commit picker should succeed immediately");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) &&
             WorkspaceShellTestAccess::ActiveOverlayMode(shell) ==
                 WorkspaceShell::OverlayMode::CommitPicker,
         "the commit picker overlay should be visible while the query is in flight");
  Expect(WorkspaceShellTestAccess::ComparePickerLoading(shell),
         "the picker should report loading before git returns");
  Expect(WorkspaceShellTestAccess::ComparePickerItemCount(shell) == 0,
         "the picker item list should be empty while loading");

  // Wait until the worker has actually entered the (blocked) first provider call
  // so the second open queues behind an in-flight job.
  const auto call_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (call_count.load() < 1 && std::chrono::steady_clock::now() < call_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(call_count.load() >= 1, "the first async history query should have started");

  // Second open supersedes the first (bumps the request generation). When both
  // complete, the stale first completion must be dropped and only the second
  // (two commits) applied.
  Expect(WorkspaceShellTestAccess::OpenComparePickerForPath(shell, source_b),
         "opening a second async commit picker should succeed");
  Expect(WorkspaceShellTestAccess::ComparePickerLoading(shell),
         "the superseding picker should also start in a loading state");

  release.store(true, std::memory_order_release);
  Expect(SettleComparePicker(shell), "both async queries should settle");
  Expect(!WorkspaceShellTestAccess::ComparePickerLoading(shell),
         "the picker should leave the loading state once the current query lands");
  Expect(WorkspaceShellTestAccess::ComparePickerItemCount(shell) == 2,
         "only the superseding (second) result should populate the picker, not the stale first");
}

void TestWorkspaceShellCompareRecomputeGate() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add compare gate fixture", "compare gate fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);

  // A refresh with no content change must reuse the existing model.
  const std::uint64_t baseline_revision = compare.model_revision;
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == baseline_revision,
         "no-op refreshes must not rebuild the compare model");

  // A left-content change must rebuild exactly once.
  compare.left_content = "int gamma() {\n  return 3;\n}\n";
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == baseline_revision + 1,
         "changed content must rebuild the compare model once");
  const std::uint64_t after_content = compare.model_revision;
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == after_content,
         "refresh after a rebuild with unchanged content must not rebuild again");

  // Toggling a build option changes the model without a content change.
  compare.build_options.ignore_whitespace = !compare.build_options.ignore_whitespace;
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == after_content + 1,
         "toggling ignore-whitespace must rebuild the compare model");

  // An edit to the editable right pane is detected via the viewport's monotonic
  // content_revision (no whole-buffer serialize on the no-op path) and rebuilds once;
  // a subsequent no-op refresh must reuse the model rather than re-serialize + rebuild.
  const std::uint64_t before_right_edit = compare.model_revision;
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "// note "),
         "typing into the editable current-state pane should edit it");
  Expect(compare.model_revision > before_right_edit,
         "editing the right pane must rebuild the compare model");
  const std::uint64_t after_right_edit = compare.model_revision;
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == after_right_edit,
         "no-op refreshes after a right-pane edit must not rebuild the compare model");
}

}  // namespace

// H11/J41: an unreadable or binary working-tree file must NOT open as an editable
// compare showing "whole file deleted" (which could then save a false empty file).
// A genuinely-missing working-tree file is a real deletion and still opens (empty).
// Distinct files per case avoid compare-tab reuse collisions on the same path.
void TestWorkspaceShellWorkingTreeCompareRejectsBinaryAndUnreadable() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path missing_file = root / "src" / "missing.cpp";
  const std::filesystem::path binary_file = root / "src" / "binary.cpp";
  const std::filesystem::path locked_file = root / "src" / "locked.cpp";
  WriteFile(missing_file, "int alpha() {\n  return 1;\n}\n");
  WriteFile(binary_file, "int beta() {\n  return 2;\n}\n");
  WriteFile(locked_file, "int gamma() {\n  return 3;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare reject fixture", "compare reject fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  // Missing working-tree file: a legitimate deletion -> opens as an (empty) diff.
  std::filesystem::remove(missing_file);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, missing_file, "HEAD", "HEAD"),
         "a missing working-tree file should still open as a whole-file-deleted diff");

  // Binary working-tree file (early AND late NUL): must be refused, not rendered as
  // deleted/empty text.
  std::string binary_bytes;
  binary_bytes.push_back('\0');            // early NUL
  binary_bytes.append("PNG\r\n\x1a\n binary body ");
  binary_bytes.push_back('\0');            // late NUL
  binary_bytes.append(" trailing");
  WriteFile(binary_file, binary_bytes);
  Expect(!WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, binary_file, "HEAD", "HEAD"),
         "a binary working-tree file must not open as an editable text compare");

  // Unreadable working-tree file: must be refused (not treated as empty). chmod 000
  // is bypassed by root, so guard the assertion when the harness runs privileged.
  std::error_code ec;
  std::filesystem::permissions(locked_file, std::filesystem::perms::none,
                               std::filesystem::perm_options::replace, ec);
  if (!ec) {
    std::ifstream probe(locked_file, std::ios::binary);
    const bool actually_locked = !probe.good();
    probe.close();
    if (actually_locked) {
      Expect(!WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, locked_file, "HEAD", "HEAD"),
             "an unreadable working-tree file must not open as an empty compare");
    }
    std::filesystem::permissions(locked_file, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
  }
}

// Regression: syntax highlighting reaches hunks deep in a collapsed large-file
// diff. The tokenizer window is derived in MODEL space from the visible
// presentation rows (a collapsed run maps a low presentation index to a high model
// row), with a continued redraw catching the cumulative tokenizer up. Previously
// the window used the presentation scroll position, capping tokenization far below
// the visible model rows, so deep rows drew unhighlighted.
void TestWorkspaceShellCompareSyntaxReachesDeepCollapsedRows() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "big.cpp";
  // A large file whose long middle is unchanged (so it collapses) with edits at the
  // top and bottom, so the bottom hunk sits at a high MODEL row behind a collapsed
  // run of a much smaller PRESENTATION height.
  std::string base;
  for (int i = 0; i < 300; ++i) {
    base += "int v" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  WriteFile(source, base);
  InitializeGitRepo(root);
  CommitAll(root, "big base fixture", "big base fixture");
  std::string edited = base;
  edited.replace(edited.find("int v0 = 0;"), std::string("int v0 = 0;").size(), "int v0 = 999;");
  edited.replace(edited.find("int v299 = 299;"), std::string("int v299 = 299;").size(),
                 "int v299 = 42;");
  WriteFile(source, edited);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "large collapsed-diff comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const std::size_t model_row_count = compare.model.rows.size();
  const std::size_t presentation_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  Expect(model_row_count > presentation_row_count + 100,
         "the unchanged middle should collapse so model rows far exceed presentation rows");

  // Scroll to the bottom so the deep (high-model-row) hunk is on screen.
  compare.scroll_row = static_cast<int>(presentation_row_count);

  // Render repeatedly to let the per-frame-capped cumulative tokenizer catch up.
  SoftwareCanvas canvas(1280, 720);
  for (int frame = 0; frame < 20; ++frame) {
    shell.Render(canvas.renderer(), 1280, 720);
  }

  auto& tokenized_compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(tokenized_compare.syntax_rows_tokenized == tokenized_compare.model.rows.size(),
         "tokenization reaches the deepest visible model row behind the collapsed run");
  // A deep content row (the bottom v299 change) is highlighted — not left blank.
  // Scan the tail (the very last model row is the phantom trailing empty line).
  bool deep_row_has_tokens = false;
  const std::size_t tail_start = tokenized_compare.model.rows.size() > 8
                                     ? tokenized_compare.model.rows.size() - 8
                                     : 0;
  for (std::size_t row = tail_start; row < tokenized_compare.model.rows.size(); ++row) {
    if (!tokenized_compare.left_tokens_by_row[row].empty() ||
        !tokenized_compare.right_tokens_by_row[row].empty()) {
      deep_row_has_tokens = true;
      break;
    }
  }
  Expect(deep_row_has_tokens,
         "a deep content row behind the collapsed run has syntax tokens (is highlighted)");
}

microide::workspace::CompareInput MakeFileCompareInput(const std::filesystem::path& path,
                                                       bool editable) {
  auto input = microide::workspace::ReadFileCompareInput(path, editable);
  Expect(input.has_value(), "plain-compare fixture file should read cleanly");
  return std::move(*input);
}

void TestWorkspaceShellPlainCompareBuildsStickyEditableTab() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path left_path = root / "a.txt";
  const std::filesystem::path right_path = root / "b.txt";
  WriteFile(left_path, "alpha\nshared\n");
  WriteFile(right_path, "beta\nshared\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenPlainComparison(shell,
                                                       MakeFileCompareInput(left_path, false),
                                                       MakeFileCompareInput(right_path, true)),
         "OpenPlainComparison of two files should succeed");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell),
         "a plain comparison should open as a compare tab");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.plain_compare, "the tab should be marked as a plain (non-git) comparison");
  Expect(compare.review_mode == microide::compare::CompareReviewMode::Plain,
         "a plain comparison should use the Plain review mode");
  Expect(compare.left_label == "a.txt" && compare.right_label == "b.txt",
         "plain-compare labels should be the two filenames");
  Expect(compare.left_path == left_path.lexically_normal() &&
             compare.right_path == right_path.lexically_normal(),
         "plain-compare should keep the distinct left/right paths");
  Expect(compare.right_editable,
         "the right pane should be editable when its side is a real file");
  Expect(!compare.model.hunks.empty(), "differing files should produce at least one hunk");

  // The mode is sticky: a derived-state refresh must not re-infer a git mode.
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.plain_compare && compare.review_mode == microide::compare::CompareReviewMode::Plain,
         "review mode should stay Plain across a derived-state refresh");
}

// TD-2026-08-06-159: opening a compare tab must not materialize either side of the
// document as owned lines. Syntax-state detection reads a bounded head (64 lines),
// and the build used to split the whole left blob into owned strings and ask the
// right buffer for a whole-document `Snapshot()` to hand that head over — together
// about a third of a large compare's open, and the snapshot is retained afterwards.
//
// Both are invisible in the output, so this pins the two counters that see them:
// the buffer's snapshot-build count for the right side, and a shebang on line 1
// with a contradicting extension for the left, which proves the head was actually
// read rather than the detection quietly skipped.
void TestWorkspaceShellCompareOpenDoesNotMaterializeEitherSide() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path left_path = root / "left.txt";
  const std::filesystem::path right_path = root / "right.txt";
  std::string left = "#!/usr/bin/env python3\n";
  std::string right = "#!/usr/bin/env python3\n";
  for (int i = 0; i < 4000; ++i) {
    left += "value_" + std::to_string(i) + " = " + std::to_string(i) + "\n";
    right += "value_" + std::to_string(i) + " = " + std::to_string(i % 97) + "\n";
  }
  WriteFile(left_path, left);
  WriteFile(right_path, right);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  microide::editor::TextBuffer::reset_snapshot_build_count();
  Expect(WorkspaceShellTestAccess::OpenPlainComparison(shell,
                                                       MakeFileCompareInput(left_path, false),
                                                       MakeFileCompareInput(right_path, true)),
         "the large plain comparison should open");
  const std::size_t snapshots = microide::editor::TextBuffer::snapshot_build_count();
  Expect(snapshots == 0,
         "opening a compare tab must not build a whole-document snapshot (built " +
             std::to_string(snapshots) + ")");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(!compare.model.hunks.empty(), "the fixture must actually differ");
  // The `.txt` extension says nothing; only the shebang can produce a definition,
  // so a non-empty id proves the head reached the detector on both sides.
  Expect(compare.left_initial_syntax_state.definition_id != 0,
         "the left side's syntax state must still be detected from its shebang");
  Expect(compare.right_initial_syntax_state.definition_id ==
             compare.left_initial_syntax_state.definition_id,
         "both sides detect the same definition from the same shebang");
}

void TestWorkspaceShellPlainCompareBufferSidesAreReadOnly() {
  WorkspaceShell shell;
  // Buffer (untitled, no path) on the right, clipboard on the left: neither is a
  // real file, so the compare is read-only even though the right side requested
  // editability.
  microide::workspace::CompareInput clipboard{
      .content = "one\ntwo\n", .label = "Clipboard", .path = {}, .editable = false};
  microide::workspace::CompareInput buffer{
      .content = "one\nTWO\n", .label = "Untitled", .path = {}, .editable = true};
  Expect(WorkspaceShellTestAccess::OpenPlainComparison(shell, std::move(clipboard),
                                                       std::move(buffer)),
         "OpenPlainComparison of buffer vs clipboard should succeed");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.plain_compare, "buffer/clipboard comparison should be plain");
  Expect(!compare.right_editable,
         "the right pane must be read-only when its side has no on-disk path");
  Expect(compare.left_label == "Clipboard" && compare.right_label == "Untitled",
         "buffer/clipboard labels should be preserved");
}

void TestWorkspaceShellPlainCompareDedupsSameFilePair() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path left_path = root / "a.txt";
  const std::filesystem::path right_path = root / "b.txt";
  WriteFile(left_path, "alpha\n");
  WriteFile(right_path, "beta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenPlainComparison(shell,
                                                       MakeFileCompareInput(left_path, false),
                                                       MakeFileCompareInput(right_path, true)),
         "first plain comparison should open");
  const std::size_t tabs_after_first = WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell);
  Expect(WorkspaceShellTestAccess::OpenPlainComparison(shell,
                                                       MakeFileCompareInput(left_path, false),
                                                       MakeFileCompareInput(right_path, true)),
         "re-opening the same file pair should succeed");
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == tabs_after_first,
         "re-opening the same file pair should activate the existing tab, not add one");
}

void TestWorkspaceShellCompareFilesCommandOpensPlainCompare() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path left_path = temp_dir.path() / "outside_a.txt";
  const std::filesystem::path right_path = temp_dir.path() / "outside_b.txt";
  WriteFile(left_path, "one\n");
  WriteFile(right_path, "TWO\n");

  // No project is opened: compare-files works on arbitrary/outside-project paths.
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::ExecuteAction(
             shell, WorkspaceShell::ActionId::CompareFiles,
             {left_path.string(), right_path.string()}),
         "compare-files command should be handled");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell),
         "compare-files should open a compare tab");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.plain_compare && compare.left_label == "outside_a.txt" &&
             compare.right_label == "outside_b.txt",
         "compare-files should build a plain comparison of the two files");
}

void TestWorkspaceShellSelectForCompareThenCompareWithSelected() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path file_a = root / "a.txt";
  const std::filesystem::path file_b = root / "b.txt";
  WriteFile(file_a, "alpha\n");
  WriteFile(file_b, "beta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell,
                                                    WorkspaceShell::ActionId::CompareWithSelected),
         "Compare with Selected should be disabled before anything is selected");

  // Right-click file A -> Select for Compare.
  WorkspaceShellTestAccess::OpenTreeContextMenuForPath(shell, microide::workspace::TreeContextTargetKind::File, file_a);
  Expect(WorkspaceShellTestAccess::ExecuteContextMenuAction(
             shell, WorkspaceShell::ActionId::SelectForCompare),
         "Select for Compare should execute");
  Expect(WorkspaceShellTestAccess::HasCompareSelection(shell),
         "selecting a file should stash a compare selection");

  // Right-click file B -> Compare with Selected.
  WorkspaceShellTestAccess::OpenTreeContextMenuForPath(shell, microide::workspace::TreeContextTargetKind::File, file_b);
  Expect(WorkspaceShellTestAccess::ExecuteContextMenuAction(
             shell, WorkspaceShell::ActionId::CompareWithSelected),
         "Compare with Selected should execute");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell),
         "Compare with Selected should open a compare tab");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.plain_compare && compare.left_label == "a.txt" && compare.right_label == "b.txt",
         "the stashed file should be the left side and the current file the right side");
}

void TestWorkspaceShellCompareWithClipboardOpensPlainCompare() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path file_a = root / "a.txt";
  WriteFile(file_a, "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetClipboardTextReader(
      shell, []() { return std::optional<std::string>("clipboard body\n"); });

  WorkspaceShellTestAccess::OpenTreeContextMenuForPath(shell, microide::workspace::TreeContextTargetKind::File, file_a);
  Expect(WorkspaceShellTestAccess::ExecuteContextMenuAction(
             shell, WorkspaceShell::ActionId::CompareWithClipboard),
         "Compare with Clipboard should execute");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell),
         "Compare with Clipboard should open a compare tab");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.plain_compare && compare.left_label == "Clipboard" &&
             compare.right_label == "a.txt",
         "clipboard should be the left side and the current file the right side");
  Expect(compare.right_editable,
         "the right pane should be editable when its side is a real file");
}

// TD-2026-08-13-201: the drag CLAMP reached compare and merge, the two behaviours
// built on top of it did not. A drag held past the bottom of a diff pane froze
// the selection at the last visible row, and a double-click there placed a bare
// caret instead of selecting the word.
void TestWorkspaceShellCompareDragAutoscrollsAndKeepsGranularity() {
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "long.txt";
  std::string base;
  std::string head;
  for (int i = 0; i < 300; ++i) {
    base += "alpha bravo " + std::to_string(i) + "\n";
    head += "alpha charlie " + std::to_string(i) + "\n";
  }
  WriteFile(source, base);
  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, head);
  CommitAll(root, "head fixture", "head fixture");

  const auto history = microide::project::CollectGitFileHistory(root, source).commits;
  Expect(history.size() == 2, "compare autoscroll fixture should have two commits");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenBranchHeadComparison(shell, source, history[1].hash, "base",
                                                            history[0].hash, "head"),
         "branch comparison should open");
  WorkspaceShellTestAccess::MarkLayoutDirty(shell);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float row_y = surface.rows_y + surface.line_height * 0.5f;
  const float right_x = surface.right_x + 24.0f;

  // --- granularity: a double-click in the right pane selects the word ---
  Expect(SendMouseDown(shell, right_x, row_y, SDL_BUTTON_LEFT, /*clicks=*/1),
         "the first click should be handled");
  Expect(SendMouseDown(shell, right_x, row_y, SDL_BUTTON_LEFT, /*clicks=*/2),
         "the double-click should be handled");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto word_selection = compare.right_viewport.selection_range();
  Expect(word_selection.has_value() && word_selection->start.column != word_selection->end.column,
         "a double-click in the compare right pane should select a word, not place a caret");
  Expect(SendMouseUp(shell, right_x, row_y, SDL_BUTTON_LEFT), "release should be handled");

  // --- autoscroll: hold the pointer past the bottom of the pane ---
  Expect(SendMouseDown(shell, right_x, row_y, SDL_BUTTON_LEFT, /*clicks=*/1),
         "pressing in the right pane should start a selection");
  Expect(SendMouseMotion(shell, right_x, layout.editor_surface.y + layout.editor_surface.h + 200.0f,
                         SDL_BUTTON_LMASK),
         "dragging below the compare surface should be handled");

  const int scroll_after_motion = WorkspaceShellTestAccess::ActiveCompare(shell).scroll_row;
  for (int tick = 0; tick < 5; ++tick) {
    const auto result = WorkspaceShellTestAccess::HandleScheduledWake(shell);
    Expect(result.handled, "an armed compare autoscroll should keep the wake busy");
  }
  Expect(WorkspaceShellTestAccess::ActiveCompare(shell).scroll_row > scroll_after_motion,
         "holding the pointer past the bottom of a diff must keep scrolling on the wake");
  Expect(SendMouseUp(shell, right_x, row_y, SDL_BUTTON_LEFT), "release should be handled");
}

// The merge half of TD-2026-08-13-201, on the result pane.
void TestWorkspaceShellMergeDragAutoscrollsAndKeepsGranularity() {
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "conflict.txt";
  std::string body;
  for (int i = 0; i < 300; ++i) {
    body += "alpha bravo " + std::to_string(i) + "\n";
  }
  WriteFile(base, body);
  WriteFile(incoming, body + "incoming tail\n");
  WriteFile(current, body + "current tail\n");
  WriteFile(output, body);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge autoscroll fixture should open");
  WorkspaceShellTestAccess::MarkLayoutDirty(shell);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const auto interaction = WorkspaceShellTestAccess::ActiveMergeInteractionLayout(shell);
  const SDL_FRect& result_rect = interaction.result.rect;
  const float x = result_rect.x + std::min(24.0f, result_rect.w * 0.25f);
  const float y = interaction.result.text.first_line_y + interaction.result.text.line_height * 0.5f;

  Expect(SendMouseDown(shell, x, y, SDL_BUTTON_LEFT, /*clicks=*/1), "first click handled");
  Expect(SendMouseDown(shell, x, y, SDL_BUTTON_LEFT, /*clicks=*/2), "double-click handled");
  const auto word_selection =
      WorkspaceShellTestAccess::ActiveMerge(shell).result_viewport.selection_range();
  Expect(word_selection.has_value() && word_selection->start.column != word_selection->end.column,
         "a double-click in the merge result pane should select a word, not place a caret");
  Expect(SendMouseUp(shell, x, y, SDL_BUTTON_LEFT), "release handled");

  Expect(SendMouseDown(shell, x, y, SDL_BUTTON_LEFT, /*clicks=*/1), "press handled");
  Expect(SendMouseMotion(shell, x, layout.editor_surface.y + layout.editor_surface.h + 200.0f,
                         SDL_BUTTON_LMASK),
         "dragging below the merge surface should be handled");

  const int scroll_after_motion = WorkspaceShellTestAccess::ActiveMerge(shell).scroll_row;
  for (int tick = 0; tick < 5; ++tick) {
    const auto result = WorkspaceShellTestAccess::HandleScheduledWake(shell);
    Expect(result.handled, "an armed merge autoscroll should keep the wake busy");
  }
  Expect(WorkspaceShellTestAccess::ActiveMerge(shell).scroll_row > scroll_after_motion,
         "holding the pointer past the bottom of a merge must keep scrolling on the wake");
  // The mirror the layout reads must not be left behind by the wake's scrolling.
  Expect(WorkspaceShellTestAccess::ActiveMerge(shell).scroll_row ==
             static_cast<int>(
                 WorkspaceShellTestAccess::ActiveMerge(shell).result_viewport.scroll_line()),
         "the merge tab's scroll mirror must track the result viewport");
  Expect(SendMouseUp(shell, x, y, SDL_BUTTON_LEFT), "release handled");
}


// TD-2026-08-13-200: `editor.wrap` used to be a dead control on the compare and
// merge surfaces -- the flag never reached their viewports, and everything on
// them assumed one document line occupies exactly one screen row. These pin the
// row model that replaced that assumption.
void TestWorkspaceShellCompareWordWrapExpandsRowsAndKeepsPanesAligned() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const std::string long_left = "int alpha() { return " + std::string(600, 'a') + "; }";
  const std::string long_right = "int beta() { return " + std::string(900, 'b') + "; }";
  WriteFile(source, long_left + "\nshort\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare wrap fixture", "compare wrap fixture");
  WriteFile(source, long_right + "\nshort\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare wrap fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const std::size_t presentation_rows =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  Expect(!WorkspaceShellTestAccess::ActiveCompareWrapActive(shell),
         "wrap is off by default on a compare tab");
  Expect(WorkspaceShellTestAccess::ActiveCompareVisualRowCount(shell) == presentation_rows,
         "with wrap off an on-screen row IS a presentation row");
  Expect(!compare.right_viewport.soft_wrap(),
         "the compare right pane starts unwrapped");

  // The setting has to reach the compare tab's own viewport: the all-tabs walk
  // used to skip every tab that was not a plain editor tab.
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.wrap", "word"),
         "editor.wrap should be settable");
  Expect(compare.right_viewport.soft_wrap(),
         "turning Word Wrap on must reach the compare tab's editable pane");

  const std::size_t wrapped_rows = WorkspaceShellTestAccess::ActiveCompareVisualRowCount(shell);
  Expect(WorkspaceShellTestAccess::ActiveCompareWrapActive(shell),
         "the compare wrap table should be live once the setting is on");
  Expect(wrapped_rows > presentation_rows,
         "wrapping a 900-column line must add on-screen rows");

  // Alignment: the presentation row holding the two long lines occupies the SAME
  // rows on both sides, and every on-screen row maps back to exactly one
  // presentation row in order.
  std::size_t previous = 0;
  for (std::size_t visual = 0; visual < wrapped_rows; ++visual) {
    const std::size_t presentation =
        WorkspaceShellTestAccess::ActiveComparePresentationRowForVisualRow(shell, visual);
    Expect(presentation >= previous, "on-screen rows must map to presentation rows in order");
    Expect(presentation < presentation_rows, "a mapped presentation row must be in range");
    previous = presentation;
  }
  for (std::size_t presentation = 0; presentation < presentation_rows; ++presentation) {
    const std::size_t first =
        WorkspaceShellTestAccess::ActiveCompareVisualRowForPresentationRow(shell, presentation);
    Expect(WorkspaceShellTestAccess::ActiveComparePresentationRowForVisualRow(shell, first) ==
               presentation,
           "a presentation row's first on-screen row must map back to it");
  }

  // Horizontal scrolling is gone while wrapped, exactly as in the editor.
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(!surface.show_horizontal,
         "a wrapped compare surface must not offer a horizontal scrollbar");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.wrap", "off"),
         "editor.wrap should be settable back off");
  Expect(!compare.right_viewport.soft_wrap(), "turning Word Wrap off must reach the compare pane");
  Expect(WorkspaceShellTestAccess::ActiveCompareVisualRowCount(shell) == presentation_rows,
         "turning wrap off must collapse the row table back to the identity");
}

void TestWorkspaceShellMergeWordWrapExpandsRows() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  const std::string long_line(900, 'x');
  WriteFile(base, "top\nbase\nbottom\n");
  WriteFile(incoming, "top\n" + long_line + "\nbottom\n");
  WriteFile(current, "top\ncurrent\nbottom\n");
  WriteFile(output, "top\ncurrent\nbottom\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for the wrap fixture");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  const std::size_t unwrapped_rows = WorkspaceShellTestAccess::ActiveMergeVisualRowCount(shell);
  Expect(!WorkspaceShellTestAccess::ActiveMergeWrapActive(shell),
         "wrap is off by default on a merge tab");
  Expect(!merge.result_viewport.soft_wrap(), "the merge result pane starts unwrapped");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.wrap", "word"),
         "editor.wrap should be settable");
  Expect(merge.result_viewport.soft_wrap(),
         "turning Word Wrap on must reach the merge tab's result pane");
  Expect(WorkspaceShellTestAccess::ActiveMergeWrapActive(shell),
         "the merge source-pane wrap table should be live once the setting is on");
  Expect(WorkspaceShellTestAccess::ActiveMergeVisualRowCount(shell) > unwrapped_rows,
         "wrapping a 900-column incoming line must add on-screen rows");

  const auto surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  Expect(!surface.show_horizontal,
         "a wrapped merge surface must not offer a horizontal scrollbar");
}

// An edit action (not typing, not paste, not undo) on a compare tab's right pane
// must rebuild the diff model: the surface paints its right text FROM that model,
// so an action that only mutated the buffer left the pre-edit text on screen.
void TestWorkspaceShellCompareEditActionRefreshesDiffModel() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "alpha\nbravo\ncharlie\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare action fixture", "compare action fixture");
  WriteFile(source, "alpha\nbravo\ncharlie\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare action fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.right_editable, "the working-tree compare pane should be editable");
  compare.right_viewport.MoveCursorTo(0, 0);

  // What the surface would paint down the right pane, in order: the diff model's
  // right side, NOT the viewport. The two must agree, or the edit is invisible.
  const auto painted_right_side = [&compare]() {
    std::string joined;
    for (const auto& row : compare.model.rows) {
      if (row.right_line > 0) {
        joined += row.right_text;
        joined += '\n';
      }
    }
    return joined;
  };
  const std::string initial_right_side = painted_right_side();
  Expect(initial_right_side.rfind("alpha\nbravo\ncharlie", 0) == 0,
         "the diff model should start out agreeing with the working-tree buffer");

  // MoveLineDown goes through the shared action layer (ActiveEditableViewport),
  // which reaches this pane -- the same layer that serves the editor surface.
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::MoveLineDown, {}),
         "MoveLineDown should be dispatched to the compare tab's editable pane");
  Expect(compare.right_viewport.lines().LineView(0) == "bravo",
         "MoveLineDown should have edited the compare pane's buffer");
  Expect(painted_right_side().rfind("bravo\nalpha\ncharlie", 0) == 0,
         "the diff model must be rebuilt after an action-driven edit, not left stale");
}

void RegisterWorkspaceShellCompareTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/CompareSyntaxReachesDeepCollapsedRows",
          TestWorkspaceShellCompareSyntaxReachesDeepCollapsedRows);
  AddTest(tests, "WorkspaceShell/CompareDragAutoscrollsAndKeepsGranularity",
          TestWorkspaceShellCompareDragAutoscrollsAndKeepsGranularity);
  AddTest(tests, "WorkspaceShell/MergeDragAutoscrollsAndKeepsGranularity",
          TestWorkspaceShellMergeDragAutoscrollsAndKeepsGranularity);
  AddTest(tests, "WorkspaceShell/WorkingTreeCompareIsEditableAndSaves",
          TestWorkspaceShellWorkingTreeCompareIsEditableAndSaves);
  AddTest(tests, "WorkspaceShell/CompareWordWrapExpandsRowsAndKeepsPanesAligned",
          TestWorkspaceShellCompareWordWrapExpandsRowsAndKeepsPanesAligned);
  AddTest(tests, "WorkspaceShell/MergeWordWrapExpandsRows",
          TestWorkspaceShellMergeWordWrapExpandsRows);
  AddTest(tests, "WorkspaceShell/CompareEditActionRefreshesDiffModel",
          TestWorkspaceShellCompareEditActionRefreshesDiffModel);
  AddTest(tests, "WorkspaceShell/WorkingTreeCompareRejectsBinaryAndUnreadable",
          TestWorkspaceShellWorkingTreeCompareRejectsBinaryAndUnreadable);
  AddTest(tests, "WorkspaceShell/CompareBracketMatchIsMemoized",
          TestWorkspaceShellCompareBracketMatchIsMemoized);
  AddTest(tests, "WorkspaceShell/CompareEditablePaneIndentsAndOutdents",
          TestWorkspaceShellCompareEditablePaneIndentsAndOutdents);
  AddTest(tests, "WorkspaceShell/CompareClickTogglesEditablePaneFocus",
          TestWorkspaceShellCompareClickTogglesEditablePaneFocus);
  AddTest(tests, "WorkspaceShell/CompareClickAboveFirstRowIsNotHandled",
          TestWorkspaceShellCompareClickAboveFirstRowIsNotHandled);
  AddTest(tests, "WorkspaceShell/CompareCollapsedContextButtonsExpandHiddenRows",
          TestWorkspaceShellCompareCollapsedContextButtonsExpandHiddenRows);
  AddTest(tests, "WorkspaceShell/CompareCollapsedContextButtonsHoverAsInteractive",
          TestWorkspaceShellCompareCollapsedContextButtonsHoverAsInteractive);
  AddTest(tests, "WorkspaceShell/CompareCollapsedContextExpansionPersistsAcrossChunks",
          TestWorkspaceShellCompareCollapsedContextExpansionPersistsAcrossChunks);
  AddTest(tests, "WorkspaceShell/CompareCollapsedContextExpansionSurvivesTreeRefresh",
          TestWorkspaceShellCompareCollapsedContextExpansionSurvivesTreeRefresh);
  AddTest(tests, "WorkspaceShell/ReadOnlyCompareRightPaneSupportsSelectAllAndCopy",
          TestWorkspaceShellReadOnlyCompareRightPaneSupportsSelectAllAndCopy);
  AddTest(tests, "WorkspaceShell/ReadOnlyCompareShortcutCopyUsesNavigableViewport",
          TestWorkspaceShellReadOnlyCompareShortcutCopyUsesNavigableViewport);
  AddTest(tests, "WorkspaceShell/CompareWheelScrollsRows",
          TestWorkspaceShellCompareWheelScrollsRows);
  AddTest(tests, "WorkspaceShell/CompareWheelScrollsColumns",
          TestWorkspaceShellCompareWheelScrollsColumns);
  AddTest(tests, "WorkspaceShell/CompareWheelScrollsLeftDominatedColumns",
          TestWorkspaceShellCompareWheelScrollsLeftDominatedColumns);
  AddTest(tests, "WorkspaceShell/CompareHorizontalNavigationInvalidatesEditablePane",
          TestWorkspaceShellCompareHorizontalNavigationInvalidatesEditablePane);
  AddTest(tests, "WorkspaceShell/CompareSelectionStepInvalidatesRowBand",
          TestWorkspaceShellCompareSelectionStepInvalidatesRowBand);
  AddTest(tests, "WorkspaceShell/CompareRenderPaintsDiagnosticGutterMarkers",
          TestWorkspaceShellCompareRenderPaintsDiagnosticGutterMarkers);
  AddTest(tests, "WorkspaceShell/CompareRenderReusesVisibleLayoutCache",
          TestWorkspaceShellCompareRenderReusesVisibleLayoutCache);
  AddTest(tests, "WorkspaceShell/CompareBlameLoadsForWorkingTreePane",
          TestWorkspaceShellCompareBlameLoadsForWorkingTreePane);
  AddTest(tests, "WorkspaceShell/MergeBlameLoadsForResultPane",
          TestWorkspaceShellMergeBlameLoadsForResultPane);
  AddTest(tests, "WorkspaceShell/CompareTabUsesFilenameOnlyLabelAndTooltip",
          TestWorkspaceShellCompareTabUsesFilenameOnlyLabelAndTooltip);
  AddTest(tests, "WorkspaceShell/MergeTabUsesFilenameOnlyLabelAndTooltip",
          TestWorkspaceShellMergeTabUsesFilenameOnlyLabelAndTooltip);
  AddTest(tests, "WorkspaceShell/MergeEditorRefusesBinaryInput",
          TestWorkspaceShellMergeEditorRefusesBinaryInput);
  AddTest(tests, "WorkspaceShell/CompareDividerMatchesMarkerWidth",
          TestWorkspaceShellCompareDividerMatchesMarkerWidth);
  AddTest(tests, "WorkspaceShell/CompareRenderKeepsDividerBorderOnUnchangedRows",
          TestWorkspaceShellCompareRenderKeepsDividerBorderOnUnchangedRows);
  AddTest(tests, "WorkspaceShell/ComparePaneResizeKeepsWiderPaneTextVisible",
          TestWorkspaceShellComparePaneResizeKeepsWiderPaneTextVisible);
  AddTest(tests, "WorkspaceShell/CompareAndMergeDividersResetOnDoubleClick",
          TestWorkspaceShellCompareAndMergeDividersResetOnDoubleClick);
  AddTest(tests, "WorkspaceShell/CompareAndMergePaneMinimaPreserveVisibleColumns",
          TestWorkspaceShellCompareAndMergePaneMinimaPreserveVisibleColumns);
  AddTest(tests, "WorkspaceShell/MergeToolbarLayoutClearsPaneHeaders",
          TestWorkspaceShellMergeToolbarLayoutClearsPaneHeaders);
  AddTest(tests, "WorkspaceShell/CompareRecomputeGate", TestWorkspaceShellCompareRecomputeGate);
  AddTest(tests, "WorkspaceShell/CommitPickerDismissesAfterOpeningCompare",
          TestWorkspaceShellCommitPickerDismissesAfterOpeningCompare);
  AddTest(tests, "WorkspaceShell/ComparePickerOpensAsyncAndDropsStaleResult",
          TestWorkspaceShellComparePickerOpensAsyncAndDropsStaleResult);
  AddTest(tests, "WorkspaceShell/PlainCompareBuildsStickyEditableTab",
          TestWorkspaceShellPlainCompareBuildsStickyEditableTab);
  AddTest(tests, "WorkspaceShell/CompareOpenDoesNotMaterializeEitherSide",
          TestWorkspaceShellCompareOpenDoesNotMaterializeEitherSide);
  AddTest(tests, "WorkspaceShell/PlainCompareBufferSidesAreReadOnly",
          TestWorkspaceShellPlainCompareBufferSidesAreReadOnly);
  AddTest(tests, "WorkspaceShell/PlainCompareDedupsSameFilePair",
          TestWorkspaceShellPlainCompareDedupsSameFilePair);
  AddTest(tests, "WorkspaceShell/CompareFilesCommandOpensPlainCompare",
          TestWorkspaceShellCompareFilesCommandOpensPlainCompare);
  AddTest(tests, "WorkspaceShell/SelectForCompareThenCompareWithSelected",
          TestWorkspaceShellSelectForCompareThenCompareWithSelected);
  AddTest(tests, "WorkspaceShell/CompareWithClipboardOpensPlainCompare",
          TestWorkspaceShellCompareWithClipboardOpensPlainCompare);
}

}  // namespace microide::tests
