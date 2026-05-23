#include "TestSupport.h"
#include "WorkspaceShellEventHelpers.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceShellTestAccess.h"

#include <chrono>
#include <filesystem>
#include <thread>

namespace microide::tests {
namespace {

using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using microide::workspace::BottomPanelResizeHandleRect;
using microide::workspace::BottomPanelResizeCursorRect;
using microide::workspace::BottomPanelResizeHitRect;
using microide::workspace::Contains;
using microide::workspace::SidebarResizeCursorRect;
using microide::workspace::SidebarResizeHandleRect;
using microide::workspace::SidebarResizeHitRect;
using microide::workspace::WindowControlButtonHitRect;
using microide::workspace::WorkspaceShell;

void WaitForProjectSearch(WorkspaceShell& shell) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    WorkspaceShellTestAccess::ConsumeProjectSearchUpdates(shell);
    if (!WorkspaceShellTestAccess::ProjectSearchRunning(shell)) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  Expect(false, "workspace project search should finish");
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

  const auto overlay_layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const SDL_FRect settings_rect =
      microide::workspace::ComputeOverlaySurfaceRect(overlay_layout.editor_area);
  const float row_x = settings_rect.x + settings_rect.w * 0.5f;
  const float row_y = settings_rect.y + 58.0f;
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

  const SDL_FRect sidebar_visual = SidebarResizeHandleRect(layout);
  const SDL_FRect sidebar_hit = SidebarResizeHitRect(layout);
  const float sidebar_x = sidebar_visual.x + sidebar_visual.w + 2.0f;
  const float sidebar_y = sidebar_visual.y + sidebar_visual.h * 0.5f;
  Expect(Contains(sidebar_hit, sidebar_x, sidebar_y),
         "sidebar seam regression should still probe a point inside the drag hit pad");
  Expect(!Contains(SidebarResizeCursorRect(layout), sidebar_x, sidebar_y),
         "sidebar seam regression should probe a point just outside the cursor rect");
  WorkspaceShellTestAccess::UpdateMouseCursor(shell, sidebar_x, sidebar_y);
  Expect(!WorkspaceShellTestAccess::CachedCursorIsEwResize(shell),
         "leaving the visible sidebar seam should clear the horizontal resize cursor promptly");
  Expect(!WorkspaceShellTestAccess::CursorKindAtIsEwResize(shell, sidebar_x, sidebar_y),
         "just outside the sidebar seam the cursor should no longer resolve to horizontal resize");

  const SDL_FRect panel_visual = BottomPanelResizeHandleRect(layout);
  const SDL_FRect panel_hit = BottomPanelResizeHitRect(layout);
  const float panel_x = panel_visual.x + panel_visual.w * 0.5f;
  const float panel_y = panel_visual.y - 2.0f;
  Expect(Contains(panel_hit, panel_x, panel_y),
         "panel seam regression should still probe a point inside the drag hit pad");
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

}  // namespace

void RegisterWorkspaceShellCursorTests(std::vector<TestCase>& tests) {
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
  AddTest(tests, "WorkspaceShell/CursorUpdatesWhenProjectSearchResultsArriveWithoutMotion",
          TestWorkspaceShellCursorUpdatesWhenProjectSearchResultsArriveWithoutMotion);
}

}  // namespace microide::tests
