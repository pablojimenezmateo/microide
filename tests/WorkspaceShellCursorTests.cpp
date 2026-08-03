#include "TestSupport.h"
#include "WorkspaceShellEventHelpers.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <chrono>
#include <filesystem>
#include <thread>

namespace microide::tests {
namespace {

using microide::workspace::StatusBarSegmentId;

using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using microide::workspace::BottomPanelResizeHandleRect;
using microide::workspace::BottomPanelResizeCursorRect;
using microide::workspace::BottomPanelResizeHitRect;
using microide::workspace::Contains;
using microide::workspace::EditorSplitOrientation;
using microide::workspace::RectsEqual;
using microide::workspace::SidebarResizeCursorRect;
using microide::workspace::SidebarResizeHandleRect;
using microide::workspace::SidebarResizeHitRect;
using microide::workspace::WindowControlButtonHitRect;
using microide::workspace::WorkspaceShell;

void WaitForProjectSearch(WorkspaceShell& shell) {
  const bool finished = WaitUntil(
      [&shell]() { return !WorkspaceShellTestAccess::ProjectSearchRunning(shell); },
      std::chrono::seconds(2), std::chrono::milliseconds(5),
      [&shell]() { WorkspaceShellTestAccess::ConsumeProjectSearchUpdates(shell); });
  Expect(finished, "workspace project search should finish");
}

void TestWorkspaceShellCursorUpdatesWhenBottomPanelHidesWithoutMotion() {
  EnsureDummySdlVideoInitialized();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::MarkLayoutDirty(shell);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const SDL_FRect panel_visual = BottomPanelResizeHandleRect(layout);
  const float x = panel_visual.x + panel_visual.w * 0.5f;
  const float y = panel_visual.y + panel_visual.h * 0.5f;
  Expect(Contains(BottomPanelResizeCursorRect(layout), x, y),
         "cursor regression should target the visible bottom-panel resize seam");

  WorkspaceShellTestAccess::UpdateMouseCursor(shell, x, y);
  Expect(WorkspaceShellTestAccess::CursorKindAtIsNsResize(shell, x, y),
         "resize hit target over the bottom panel should resolve to the vertical resize cursor");
  Expect(WorkspaceShellTestAccess::CachedCursorIsNsResize(shell),
         "resize hit target over the bottom panel should use the vertical resize cursor");
  Expect(WorkspaceShellTestAccess::CursorKindAtIsNsResize(shell, x, y),
         "hit testing should agree with the cached cursor over the panel divider");

  WorkspaceShellTestAccess::CloseTerminalTab(shell, 0);

  WorkspaceShellTestAccess::UpdateMouseCursor(shell, x, y);
  Expect(!WorkspaceShellTestAccess::CachedCursorIsNsResize(shell),
         "hiding the bottom panel should clear a stale vertical resize cursor without pointer motion");
  Expect(WorkspaceShellTestAccess::CursorKindAtIsText(shell, x, y),
         "the same point should now resolve to editor text once the panel is gone");
}

void TestWorkspaceShellCursorRestoresAfterMouseLeave() {
  EnsureDummySdlVideoInitialized();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::MarkLayoutDirty(shell);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const SDL_FRect panel_visual = BottomPanelResizeHandleRect(layout);
  const float x = panel_visual.x + panel_visual.w * 0.5f;
  const float y = panel_visual.y + panel_visual.h * 0.5f;
  Expect(Contains(BottomPanelResizeCursorRect(layout), x, y),
         "cursor regression should target the visible bottom-panel resize seam");

  (void)SendMouseMotion(shell, x, y, static_cast<SDL_MouseButtonFlags>(0));
  Expect(WorkspaceShellTestAccess::CachedCursorIsNsResize(shell),
         "motion over the panel divider should select the vertical resize cursor");

  (void)SendWindowMouseLeave(shell);
  Expect(WorkspaceShellTestAccess::CachedCursorIsDefault(shell),
         "mouse leave should reset the cached cursor kind");

  (void)SendMouseMotion(shell, x, y, static_cast<SDL_MouseButtonFlags>(0));
  Expect(WorkspaceShellTestAccess::CachedCursorIsNsResize(shell),
         "returning to the divider after mouse leave should restore the resize cursor");
}

void TestWorkspaceShellSettingsOverlayCursorKind() {
  EnsureDummySdlVideoInitialized();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const float outside_x = layout.editor_surface.x + 4.0f;
  const float outside_y = layout.editor_surface.y + 4.0f;

  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);
  WorkspaceShellTestAccess::UpdateMouseCursor(shell, outside_x, outside_y);
  Expect(WorkspaceShellTestAccess::CursorKindAtIsDefault(shell, outside_x, outside_y),
         "settings overlay should force the default cursor over the dimmed editor backdrop");

  // Probe the first real value row rather than a hand-computed offset from the
  // card: the header/filter/section band above the rows has changed height more
  // than once, and an offset probe silently drifts onto whatever sits there.
  const SDL_FRect first_row = WorkspaceShellTestAccess::SettingsOverlayVisibleRowRect(shell, 0);
  Expect(first_row.w > 0.0f && first_row.h > 0.0f,
         "the settings overlay should expose at least one visible value row");
  const float row_x = first_row.x + first_row.w * 0.5f;
  const float row_y = first_row.y + first_row.h * 0.5f;
  WorkspaceShellTestAccess::UpdateMouseCursor(shell, row_x, row_y);
  Expect(WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, row_x, row_y),
         "settings rows should expose a pointer cursor");
}

void TestWorkspaceShellResizeCursorFallsBackOutsideVisibleSeams() {
  EnsureDummySdlVideoInitialized();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::MarkLayoutDirty(shell);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);

  // The grab (hit) region must equal the cursor-change region exactly: now that the
  // cursor reliably refreshes, a point outside the cursor rect is outside the drag pad
  // too — there is no longer a band where you can grab but the cursor stays an arrow.
  const SDL_FRect sidebar_visual = SidebarResizeHandleRect(layout);
  Expect(RectsEqual(SidebarResizeHitRect(layout), SidebarResizeCursorRect(layout)),
         "sidebar resize hit rect must equal the cursor rect (no over-extended grab pad)");
  const float sidebar_x = sidebar_visual.x + sidebar_visual.w + 2.0f;
  const float sidebar_y = sidebar_visual.y + sidebar_visual.h * 0.5f;
  Expect(!Contains(SidebarResizeHitRect(layout), sidebar_x, sidebar_y),
         "just outside the seam should fall outside the drag pad");
  Expect(!Contains(SidebarResizeCursorRect(layout), sidebar_x, sidebar_y),
         "sidebar seam regression should probe a point just outside the cursor rect");
  WorkspaceShellTestAccess::UpdateMouseCursor(shell, sidebar_x, sidebar_y);
  Expect(!WorkspaceShellTestAccess::CachedCursorIsEwResize(shell),
         "leaving the visible sidebar seam should clear the horizontal resize cursor promptly");
  Expect(!WorkspaceShellTestAccess::CursorKindAtIsEwResize(shell, sidebar_x, sidebar_y),
         "just outside the sidebar seam the cursor should no longer resolve to horizontal resize");

  const SDL_FRect panel_visual = BottomPanelResizeHandleRect(layout);
  Expect(RectsEqual(BottomPanelResizeHitRect(layout), BottomPanelResizeCursorRect(layout)),
         "panel resize hit rect must equal the cursor rect (no over-extended grab pad)");
  const float panel_x = panel_visual.x + panel_visual.w * 0.5f;
  const float panel_y = panel_visual.y - 2.0f;
  Expect(!Contains(BottomPanelResizeHitRect(layout), panel_x, panel_y),
         "just outside the seam should fall outside the drag pad");
  Expect(!Contains(BottomPanelResizeCursorRect(layout), panel_x, panel_y),
         "panel seam regression should probe a point just outside the cursor rect");
  WorkspaceShellTestAccess::UpdateMouseCursor(shell, panel_x, panel_y);
  Expect(!WorkspaceShellTestAccess::CachedCursorIsNsResize(shell),
         "leaving the visible bottom-panel seam should clear the resize cursor promptly");
  Expect(WorkspaceShellTestAccess::CursorKindAtIsText(shell, panel_x, panel_y),
         "just above the panel seam the editor should reclaim the text cursor");
}

void TestWorkspaceShellWindowControlCursorUsesPaddedHitRect() {
  EnsureDummySdlVideoInitialized();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  const auto minimize_rect = WorkspaceShellTestAccess::MinimizeButtonRect(shell);
  Expect(minimize_rect.has_value(),
         "custom window chrome should expose a minimize button for cursor regression testing");
  const float x = minimize_rect->x - 1.0f;
  const float y = minimize_rect->y + minimize_rect->h * 0.5f;
  Expect(!Contains(*minimize_rect, x, y),
         "window-control regression should probe just outside the painted button");
  Expect(Contains(WindowControlButtonHitRect(*minimize_rect), x, y),
         "window-control regression should stay inside the padded hit rect");

  WorkspaceShellTestAccess::UpdateMouseCursor(shell, x, y);
  Expect(WorkspaceShellTestAccess::CachedCursorIsPointer(shell),
         "window controls should acquire the pointer cursor slightly outside the painted glyph box");
  Expect(WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, x, y),
         "window-control hit testing should use the padded hover target");
}

void TestWorkspaceShellCustomFrameResizeCursorsMatchHitTest() {
  EnsureDummySdlVideoInitialized();

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);
  WorkspaceShellTestAccess::MarkLayoutDirty(shell);

  const auto expect_cursor = [&](float x,
                                 float y,
                                 SDL_HitTestResult expected_hit,
                                 bool (*kind_at)(WorkspaceShell&, float, float),
                                 bool (*cached_kind)(const WorkspaceShell&),
                                 const char* message) {
    Expect(shell.WindowHitTest(x, y) == expected_hit, message);
    WorkspaceShellTestAccess::UpdateMouseCursor(shell, x, y);
    Expect(kind_at(shell, x, y),
           "custom frame hit-test and cursor kind should agree");
    Expect(cached_kind(shell),
           "custom frame cursor should be cached after updating the mouse cursor");
  };

  expect_cursor(1.0f, 1.0f, SDL_HITTEST_RESIZE_TOPLEFT,
                WorkspaceShellTestAccess::CursorKindAtIsNwResize,
                WorkspaceShellTestAccess::CachedCursorIsNwResize,
                "top-left custom frame edge should resize diagonally");
  expect_cursor(1278.0f, 1.0f, SDL_HITTEST_RESIZE_TOPRIGHT,
                WorkspaceShellTestAccess::CursorKindAtIsNeResize,
                WorkspaceShellTestAccess::CachedCursorIsNeResize,
                "top-right custom frame edge should resize diagonally");
  expect_cursor(1278.0f, 718.0f, SDL_HITTEST_RESIZE_BOTTOMRIGHT,
                WorkspaceShellTestAccess::CursorKindAtIsSeResize,
                WorkspaceShellTestAccess::CachedCursorIsSeResize,
                "bottom-right custom frame edge should resize diagonally");
  expect_cursor(1.0f, 718.0f, SDL_HITTEST_RESIZE_BOTTOMLEFT,
                WorkspaceShellTestAccess::CursorKindAtIsSwResize,
                WorkspaceShellTestAccess::CachedCursorIsSwResize,
                "bottom-left custom frame edge should resize diagonally");
  expect_cursor(640.0f, 1.0f, SDL_HITTEST_RESIZE_TOP,
                WorkspaceShellTestAccess::CursorKindAtIsNResize,
                WorkspaceShellTestAccess::CachedCursorIsNResize,
                "top custom frame edge should use a vertical resize cursor");
  expect_cursor(1278.0f, 360.0f, SDL_HITTEST_RESIZE_RIGHT,
                WorkspaceShellTestAccess::CursorKindAtIsEResize,
                WorkspaceShellTestAccess::CachedCursorIsEResize,
                "right custom frame edge should use a horizontal resize cursor");
  expect_cursor(640.0f, 718.0f, SDL_HITTEST_RESIZE_BOTTOM,
                WorkspaceShellTestAccess::CursorKindAtIsSResize,
                WorkspaceShellTestAccess::CachedCursorIsSResize,
                "bottom custom frame edge should use a vertical resize cursor");
  expect_cursor(1.0f, 360.0f, SDL_HITTEST_RESIZE_LEFT,
                WorkspaceShellTestAccess::CursorKindAtIsWResize,
                WorkspaceShellTestAccess::CachedCursorIsWResize,
                "left custom frame edge should use a horizontal resize cursor");
}

// The status bar's segments carry commands and answer clicks.
//
// This test previously asserted the opposite -- "status bar segments must not be
// clickable" -- but that pinned a defect rather than a decision. Every segment
// shipped with an imperative tooltip ("Go to Line", "Open Problems", "Open Source
// Control") while `clickable` was never set anywhere and no mouse-down path ever
// looked at the bar, so the tooltips promised actions nothing implemented. Users
// read them as a menu and clicked nothing.
void TestWorkspaceShellStatusBarSegmentsDispatchTheirCommands() {
  EnsureDummySdlVideoInitialized();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::RefreshStatusBar(shell);

  // An actionable segment shows the pointer cursor, like every other control.
  const auto project_rect =
      WorkspaceShellTestAccess::StatusBarSegmentRect(shell, StatusBarSegmentId::Project);
  Expect(project_rect.has_value(), "the project segment should be present at this width");
  const float px = project_rect->x + project_rect->w * 0.5f;
  const float py = project_rect->y + project_rect->h * 0.5f;
  Expect(WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, px, py),
         "a segment that runs a command should show the pointer cursor");

  // And clicking it runs that command -- here, opening Source Control, which is
  // exactly what its tooltip has always claimed.
  Expect(SendMouseDown(shell, px, py, SDL_BUTTON_LEFT),
         "a click on an actionable status bar segment should be handled");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Git,
         "clicking the project segment should open the Source Control view");

  // A read-only segment stays read-only: no command, no pointer, and the click
  // falls through rather than being swallowed by the bar.
  const auto encoding_rect =
      WorkspaceShellTestAccess::StatusBarSegmentRect(shell, StatusBarSegmentId::Encoding);
  if (encoding_rect.has_value()) {
    Expect(WorkspaceShellTestAccess::CursorKindAtIsDefault(
               shell, encoding_rect->x + encoding_rect->w * 0.5f,
               encoding_rect->y + encoding_rect->h * 0.5f),
           "a segment with no command must not advertise itself as a control");
  }
}

void TestWorkspaceShellCursorUpdatesWhenProjectSearchResultsArriveWithoutMotion() {
  EnsureDummySdlVideoInitialized();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 1,
         "search cursor fixture should expose one result");
  const SDL_FRect result_rect = WorkspaceShellTestAccess::ProjectSearchResultRect(shell, 0);
  const float x = result_rect.x + result_rect.w * 0.5f;
  const float y = result_rect.y + result_rect.h * 0.5f;

  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "nomatch", false);
  WorkspaceShellTestAccess::UpdateMouseCursor(shell, x, y);
  Expect(WorkspaceShellTestAccess::CachedCursorIsDefault(shell),
         "clearing search results should reset the cached cursor over the old result row");
  Expect(WorkspaceShellTestAccess::CursorKindAtIsDefault(shell, x, y),
         "without search results the old result row should no longer resolve to a pointer");
  WaitForProjectSearch(shell);
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).empty(),
         "nomatch query should leave the search result list empty");

  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WorkspaceShellTestAccess::UpdateMouseCursor(shell, x, y);
  Expect(WorkspaceShellTestAccess::CachedCursorIsDefault(shell),
         "before result updates arrive the cached cursor should remain default");
  WaitForProjectSearch(shell);
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 1,
         "rerunning the search should repopulate the result row under the mouse");

  WorkspaceShellTestAccess::UpdateMouseCursor(shell, x, y);
  Expect(WorkspaceShellTestAccess::CachedCursorIsPointer(shell),
         "sidebar redraws that repopulate search results should invalidate the cached cursor");
  Expect(WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, x, y),
         "the repopulated search result row should resolve to the pointer cursor");
}

void TestWorkspaceShellCursorSplitNonFocusedGroupTabUsesPointer() {
  EnsureDummySdlVideoInitialized();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "splitting the editor should succeed with an active tab");
  Expect(WorkspaceShellTestAccess::EditorGroupCount(shell) == 2,
         "the split should create a second editor group");
  // Splitting focuses the new (right) group, so group 0 is the non-focused one whose
  // tabs the old focused-group-only cursor hit-test computed against the wrong strip.
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1,
         "the split should focus the new group");

  const SDL_FRect tab_rect = WorkspaceShellTestAccess::GroupEditorTabRect(shell, 0, 0);
  Expect(tab_rect.w > 0.0f && tab_rect.h > 0.0f,
         "the non-focused split group should expose its own tab rect");
  const float x = tab_rect.x + tab_rect.w * 0.5f;
  const float y = tab_rect.y + tab_rect.h * 0.5f;

  WorkspaceShellTestAccess::UpdateMouseCursor(shell, x, y);
  Expect(WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, x, y),
         "a tab in the non-focused split group should resolve to the pointer cursor");
  Expect(WorkspaceShellTestAccess::CachedCursorIsPointer(shell),
         "hovering a non-focused split-group tab should cache the pointer cursor");
}

// The command palette stores match rows as INDICES into `items` (not copied rows) to
// avoid copying every matched row's strings on each keystroke (TD-2026-07-17A-032). This
// pins that the index indirection resolves to the correct rows: with no query every match
// maps 1:1 to its item, and after filtering, the surviving match still resolves to a row
// whose label actually contains the query.
void TestWorkspaceShellCommandPaletteMatchesIndexIntoItems() {
  EnsureDummySdlVideoInitialized();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::OpenCommandPalette(shell);

  // No query: every item is a match, and matches[i] must resolve back to items[i].
  const std::size_t item_count = WorkspaceShellTestAccess::CommandPaletteItemCount(shell);
  Expect(item_count > 0, "the command palette should populate built-in command rows");
  Expect(WorkspaceShellTestAccess::CommandPaletteMatchCount(shell) == item_count,
         "with no query every item is a match");
  for (std::size_t i = 0; i < item_count; ++i) {
    Expect(WorkspaceShellTestAccess::CommandPaletteMatchLabelAt(shell, i) ==
               WorkspaceShellTestAccess::CommandPaletteItemLabelAt(shell, i),
           "each unfiltered match index must resolve to the same-position item");
  }

  // Filter by the first item's exact label: it must keep that row (querying a row's own
  // label always matches its search text), never grow the set, and every surviving match
  // must resolve through a valid index to a non-empty label.
  const std::string first_label = WorkspaceShellTestAccess::CommandPaletteItemLabelAt(shell, 0);
  Expect(!first_label.empty(), "the first palette item should carry a label");
  WorkspaceShellTestAccess::SetCommandPaletteQueryAndRefresh(shell, first_label);
  const std::size_t narrowed = WorkspaceShellTestAccess::CommandPaletteMatchCount(shell);
  Expect(narrowed >= 1 && narrowed <= item_count,
         "an exact-label query keeps at least the matching row and never grows the set");
  bool first_label_survives = false;
  for (std::size_t i = 0; i < narrowed; ++i) {
    const std::string label = WorkspaceShellTestAccess::CommandPaletteMatchLabelAt(shell, i);
    Expect(!label.empty(), "every surviving match must resolve to a valid item row");
    if (label == first_label) {
      first_label_survives = true;
    }
  }
  Expect(first_label_survives,
         "querying a row's own label must keep that row among the matches");
}

void TestWorkspaceShellCursorRecomputesWhenCommandPaletteOpensWithoutMotion() {
  EnsureDummySdlVideoInitialized();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const float x = layout.editor_surface.x + layout.editor_surface.w * 0.5f;
  const float y = layout.editor_surface.y + layout.editor_surface.h * 0.5f;

  WorkspaceShellTestAccess::UpdateMouseCursor(shell, x, y);
  const int before = WorkspaceShellTestAccess::CachedCursorValue(shell);

  WorkspaceShellTestAccess::OpenCommandPalette(shell);
  const int fresh_after = WorkspaceShellTestAccess::CursorKindAtValue(shell, x, y);
  Expect(fresh_after != before,
         "command palette open should change the resolved cursor kind at the test point");

  // The collapsed cursor fingerprint dropped the overlay-visible scalar, so a
  // stationary-pointer recompute now relies solely on the overlay-open path bumping
  // the cursor hit generation. Without motion, the cached cursor must still update.
  WorkspaceShellTestAccess::UpdateMouseCursor(shell, x, y);
  Expect(WorkspaceShellTestAccess::CachedCursorValue(shell) == fresh_after,
         "opening the command palette must recompute the stationary cursor (no stale fast path)");
}

void TestWorkspaceShellRenderDoesNotPollLivePointer() {
  EnsureDummySdlVideoInitialized();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  // No mouse event has been delivered, so the pointer position must stay invalid
  // across renders. The render path must never poll the live OS pointer
  // (SDL_GetMouseState) to seed it — that would make hover highlights, and thus
  // retained-vs-full redraw equivalence, depend on the real cursor location.
  Expect(!WorkspaceShellTestAccess::MousePositionValid(shell),
         "a freshly opened workspace should have no cached pointer position");
  WorkspaceShellTestAccess::RenderFrame(shell);
  Expect(!WorkspaceShellTestAccess::MousePositionValid(shell),
         "rendering must not seed the pointer position from a live OS poll");
  WorkspaceShellTestAccess::RenderFrame(shell);
  Expect(!WorkspaceShellTestAccess::MousePositionValid(shell),
         "repeated renders must remain free of any live pointer poll");
}

// Regression: an event-time cursor change must request a present. On Wayland the
// cursor shape rides a hardware plane that only re-latches on a frame commit, so a
// cursor change that dirties nothing else (hovering an item with no hover visual,
// or an idle welcome screen with no caret blink) would sit queued and show the
// stale shape until some unrelated repaint. While the caret blinks its periodic
// frames hide this; when it stops, the staleness becomes visible. See
// dev-docs/platform/wayland-stale-cursor.md.
void TestWorkspaceShellCursorChangeRequestsPresent() {
  EnsureDummySdlVideoInitialized();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::MarkLayoutDirty(shell);

  // The left window-frame border reliably resolves to a non-default (resize)
  // cursor with custom chrome, so moving onto it from the default cursor is a
  // guaranteed cursor change.
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const float border_x = 2.0f;
  const float border_y = layout.menu_bar.y + layout.menu_bar.h + 200.0f;

  // Drain any redraws queued during setup so the assertions see only cursor work.
  (void)WorkspaceShellTestAccess::ConsumePendingRedraw(shell);

  // Moving onto the resize border changes the cursor (Default -> a resize cursor).
  // That change must request a present so an idle compositor recomposites and
  // shows it.
  WorkspaceShellTestAccess::UpdateMouseCursor(shell, border_x, border_y);
  Expect(!WorkspaceShellTestAccess::CachedCursorIsDefault(shell),
         "the left window frame border should resolve to a resize cursor");
  Expect(WorkspaceShellTestAccess::ConsumePendingRedraw(shell),
         "an event-time cursor change must request a present to re-latch the cursor plane");

  // Re-resolving the same point changes nothing, so it must not request a present:
  // the fix adds no per-motion cost once the cursor is stable.
  WorkspaceShellTestAccess::UpdateMouseCursor(shell, border_x, border_y);
  Expect(!WorkspaceShellTestAccess::ConsumePendingRedraw(shell),
         "an unchanged cursor must not request a present");
}

// Toasts were painted and never hit-tested: a click on one fell straight through
// to whatever it covered — over the editor, that moved the caret — and there was
// no way to get rid of a message before its four seconds were up. Both the click
// and the cursor now resolve through the same NotificationLayout helper the
// painter uses.
void TestWorkspaceShellNotificationToastIsClickable() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  std::string body;
  for (int i = 0; i < 400; ++i) {
    body += "int line_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  WriteFile(source, body);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::ShowNotification(
      shell, microide::workspace::NotificationService::Tone::Error, "build failed");
  Expect(WorkspaceShellTestAccess::ActiveNotifications(shell).size() == 1,
         "the toast fixture should post one notification");

  const SDL_FRect toast = WorkspaceShellTestAccess::NotificationToastRect(shell, 0);
  Expect(toast.w > 0.0f && toast.h > 0.0f, "the toast should have a real card rect");
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  Expect(Contains(layout.editor_surface, toast.x + toast.w * 0.5f, toast.y + toast.h * 0.5f),
         "the toast fixture should float over the editor, so a fall-through is observable");

  const float toast_x = toast.x + toast.w * 0.5f;
  const float toast_y = toast.y + toast.h * 0.5f;
  Expect(WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, toast_x, toast_y),
         "a dismissable toast should show the pointer cursor");

  const std::size_t line_before =
      WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).cursor_line();
  const std::size_t column_before =
      WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).cursor_column();
  Expect(SendMouseDown(shell, toast_x, toast_y, SDL_BUTTON_LEFT),
         "clicking a toast should be handled");
  Expect(WorkspaceShellTestAccess::ActiveNotifications(shell).empty(),
         "clicking a toast should dismiss it");
  Expect(WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).cursor_line() == line_before &&
             WorkspaceShellTestAccess::GroupActiveViewport(shell, 0).cursor_column() ==
                 column_before,
         "a click consumed by a toast must not fall through to the editor underneath");
}

}  // namespace

void RegisterWorkspaceShellCursorTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/NotificationToastIsClickable",
          TestWorkspaceShellNotificationToastIsClickable);
  AddTest(tests, "WorkspaceShell/CursorUpdatesWhenBottomPanelHidesWithoutMotion",
          TestWorkspaceShellCursorUpdatesWhenBottomPanelHidesWithoutMotion);
  AddTest(tests, "WorkspaceShell/CursorRestoresAfterMouseLeave",
          TestWorkspaceShellCursorRestoresAfterMouseLeave);
  AddTest(tests, "WorkspaceShell/SettingsOverlayCursorKind",
          TestWorkspaceShellSettingsOverlayCursorKind);
  AddTest(tests, "WorkspaceShell/ResizeCursorFallsBackOutsideVisibleSeams",
          TestWorkspaceShellResizeCursorFallsBackOutsideVisibleSeams);
  AddTest(tests, "WorkspaceShell/WindowControlCursorUsesPaddedHitRect",
          TestWorkspaceShellWindowControlCursorUsesPaddedHitRect);
  AddTest(tests, "WorkspaceShell/CustomFrameResizeCursorsMatchHitTest",
          TestWorkspaceShellCustomFrameResizeCursorsMatchHitTest);
  AddTest(tests, "WorkspaceShell/StatusBarSegmentsDispatchTheirCommands",
          TestWorkspaceShellStatusBarSegmentsDispatchTheirCommands);
  AddTest(tests, "WorkspaceShell/CursorUpdatesWhenProjectSearchResultsArriveWithoutMotion",
          TestWorkspaceShellCursorUpdatesWhenProjectSearchResultsArriveWithoutMotion);
  AddTest(tests, "WorkspaceShell/CursorSplitNonFocusedGroupTabUsesPointer",
          TestWorkspaceShellCursorSplitNonFocusedGroupTabUsesPointer);
  AddTest(tests, "WorkspaceShell/CursorRecomputesWhenCommandPaletteOpensWithoutMotion",
          TestWorkspaceShellCursorRecomputesWhenCommandPaletteOpensWithoutMotion);
  AddTest(tests, "WorkspaceShell/CommandPaletteMatchesIndexIntoItems",
          TestWorkspaceShellCommandPaletteMatchesIndexIntoItems);
  AddTest(tests, "WorkspaceShell/RenderDoesNotPollLivePointer",
          TestWorkspaceShellRenderDoesNotPollLivePointer);
  AddTest(tests, "WorkspaceShell/CursorChangeRequestsPresent",
          TestWorkspaceShellCursorChangeRequestsPresent);
}

}  // namespace microide::tests
