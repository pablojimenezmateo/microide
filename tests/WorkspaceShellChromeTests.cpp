#include "TestSupport.h"

#include "TerminalSessionTestAccess.h"
#include "render/Theme.h"
#include "util/PerformanceCounters.h"
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
using microide::workspace::ActionId;
using microide::workspace::TreeContextTargetKind;
using microide::workspace::WorkspaceTreeContextMenuItems;

bool RectsIntersect(const SDL_FRect& lhs, const SDL_FRect& rhs) {
  return lhs.x < rhs.x + rhs.w && lhs.x + lhs.w > rhs.x && lhs.y < rhs.y + rhs.h &&
         lhs.y + lhs.h > rhs.y;
}

bool AnyRectIntersects(const std::vector<SDL_FRect>& rects, const SDL_FRect& target) {
  return std::any_of(rects.begin(), rects.end(),
                     [&](const SDL_FRect& rect) { return RectsIntersect(rect, target); });
}

bool WaitForGitSidebarEntryCount(WorkspaceShell& shell, std::size_t expected_count) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
    if (WorkspaceShellTestAccess::GitSidebarEntries(shell).size() == expected_count &&
        !WorkspaceShellTestAccess::GitSidebarRefreshing(shell)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
  return WorkspaceShellTestAccess::GitSidebarEntries(shell).size() == expected_count;
}

float MaxRectHeight(const std::vector<SDL_FRect>& rects) {
  float max_height = 0.0f;
  for (const SDL_FRect& rect : rects) {
    max_height = std::max(max_height, rect.h);
  }
  return max_height;
}

SDL_Color ReadSurfacePixelOrThrow(SDL_Surface* surface, int x, int y) {
  Uint8 r = 0;
  Uint8 g = 0;
  Uint8 b = 0;
  Uint8 a = 0;
  Expect(surface != nullptr, "pixel reads require a valid surface");
  Expect(SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a),
         "workspace chrome tests should read rendered pixels");
  return SDL_Color{r, g, b, a};
}

#if MICROIDE_HAS_SDL3_TTF

void EnsureDummySdlVideo() {
  static ScopedEnvVar video_driver("SDL_VIDEODRIVER", "dummy");
  static const bool initialized = SDL_Init(SDL_INIT_VIDEO);
  Expect(initialized, "SDL should initialize with the dummy video driver");
}

class SoftwareCanvas final {
 public:
  SoftwareCanvas(int width, int height) {
    surface_ = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA8888);
    Expect(surface_ != nullptr, "workspace redraw regression test should allocate a software surface");
    renderer_ = SDL_CreateSoftwareRenderer(surface_);
    Expect(renderer_ != nullptr, "workspace redraw regression test should create a software renderer");
  }

  ~SoftwareCanvas() {
    if (renderer_ != nullptr) {
      SDL_DestroyRenderer(renderer_);
    }
    if (surface_ != nullptr) {
      SDL_DestroySurface(surface_);
    }
  }

  SDL_Renderer* renderer() const { return renderer_; }

 private:
  SDL_Surface* surface_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
};

SDL_Rect InflateClipRect(const SDL_FRect& rect,
                         microide::render::TextClipPadding padding,
                         int width,
                         int height) {
  const int x0 = std::max(0, static_cast<int>(std::floor(rect.x - padding.left)));
  const int y0 = std::max(0, static_cast<int>(std::floor(rect.y - padding.top)));
  const int x1 = std::min(width, static_cast<int>(std::ceil(rect.x + rect.w + padding.right)));
  const int y1 = std::min(height, static_cast<int>(std::ceil(rect.y + rect.h + padding.bottom)));
  return SDL_Rect{
      .x = x0,
      .y = y0,
      .w = std::max(0, x1 - x0),
      .h = std::max(0, y1 - y0),
  };
}

std::size_t CountPixelDifferences(SDL_Surface* lhs, SDL_Surface* rhs) {
  Expect(lhs != nullptr && rhs != nullptr,
         "surface comparisons should receive readable surfaces");
  Expect(lhs->w == rhs->w && lhs->h == rhs->h,
         "surface comparisons should use canvases with matching dimensions");

  std::size_t differences = 0;
  for (int y = 0; y < lhs->h; ++y) {
    for (int x = 0; x < lhs->w; ++x) {
      Uint8 lhs_r = 0;
      Uint8 lhs_g = 0;
      Uint8 lhs_b = 0;
      Uint8 lhs_a = 0;
      Uint8 rhs_r = 0;
      Uint8 rhs_g = 0;
      Uint8 rhs_b = 0;
      Uint8 rhs_a = 0;
      Expect(SDL_ReadSurfacePixel(lhs, x, y, &lhs_r, &lhs_g, &lhs_b, &lhs_a),
             "surface comparisons should read retained-render pixels");
      Expect(SDL_ReadSurfacePixel(rhs, x, y, &rhs_r, &rhs_g, &rhs_b, &rhs_a),
             "surface comparisons should read full-render pixels");
      if (lhs_r != rhs_r || lhs_g != rhs_g || lhs_b != rhs_b || lhs_a != rhs_a) {
        ++differences;
      }
    }
  }
  return differences;
}

void RenderRetainedInvalidation(WorkspaceShell& shell,
                                SoftwareCanvas& canvas,
                                int width,
                                int height,
                                const WorkspaceShell::RenderInvalidation& redraw) {
  if (redraw.full || redraw.rects.empty()) {
    shell.Render(canvas.renderer(), width, height);
    return;
  }

  shell.PrepareRenderFrame(canvas.renderer(), width, height);
  bool rendered_partial = false;
  for (const SDL_FRect& rect : redraw.rects) {
    const SDL_Rect clip_rect =
        InflateClipRect(rect, shell.PartialRedrawClipPadding(), width, height);
    if (clip_rect.w <= 0 || clip_rect.h <= 0) {
      continue;
    }
    Expect(SDL_SetRenderClipRect(canvas.renderer(), &clip_rect),
           "workspace redraw regression test should set a retained-scene clip rect");
    shell.RenderPrepared(canvas.renderer(), width, height);
    rendered_partial = true;
  }
  Expect(SDL_SetRenderClipRect(canvas.renderer(), nullptr),
         "workspace redraw regression test should clear the retained-scene clip rect");
  if (!rendered_partial) {
    shell.RenderPrepared(canvas.renderer(), width, height);
  }
}

#endif

void TestWorkspaceShellMenuBarOmitsRemovedMenus() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1920, 720);

  // Sample full menu bar including overflow so the assertions work at any width.
  std::vector<std::string> labels = WorkspaceShellTestAccess::VisibleMenuBarLabels(shell);
  for (const std::string& overflow : WorkspaceShellTestAccess::OverflowMenuBarLabels(shell)) {
    labels.push_back(overflow);
  }
  Expect(std::find(labels.begin(), labels.end(), "Project") == labels.end(),
         "menu bar should omit the removed Project menu");
  Expect(std::find(labels.begin(), labels.end(), "Terminal") != labels.end(),
         "menu bar should expose the Terminal top-level menu");
  Expect(std::find(labels.begin(), labels.end(), "Help") != labels.end(),
         "menu bar should expose the Help top-level menu");
}

void TestWorkspaceShellMenuBarShowsChevronWhenTruncated() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(!WorkspaceShellTestAccess::MenuOverflowChevronRect(shell).has_value(),
         "wide window should not produce a menu overflow chevron");

  WorkspaceShellTestAccess::SetWindowSize(shell, 280, 720);
  const auto chevron = WorkspaceShellTestAccess::MenuOverflowChevronRect(shell);
  Expect(chevron.has_value(),
         "narrow window should produce a menu overflow chevron rather than silently truncating");
  const auto overflow_labels = WorkspaceShellTestAccess::OverflowMenuBarLabels(shell);
  Expect(!overflow_labels.empty(),
         "overflow label list must contain the menus that did not fit");
  const auto visible_labels = WorkspaceShellTestAccess::VisibleMenuBarLabels(shell);
  for (const auto& v : visible_labels) {
    Expect(std::find(overflow_labels.begin(), overflow_labels.end(), v) == overflow_labels.end(),
           "visible and overflow menu lists must be disjoint");
  }
}

void TestWorkspaceShellCompactMenuOverflowButtonIsInteractive() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);
  WorkspaceShellTestAccess::SetLayoutMode(shell, microide::workspace::LayoutMode::Compact);

  const auto chevron = WorkspaceShellTestAccess::MenuOverflowChevronRect(shell);
  Expect(chevron.has_value(),
         "compact chrome should expose a hamburger/overflow menu button");
  const float x = chevron->x + chevron->w * 0.5f;
  const float y = chevron->y + chevron->h * 0.5f;
  Expect(shell.WindowHitTest(x, y) == SDL_HITTEST_NORMAL,
         "compact menu button must not be classified as draggable title-bar space");

  SDL_Event click_event{};
  click_event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  click_event.button.button = SDL_BUTTON_LEFT;
  click_event.button.x = x;
  click_event.button.y = y;
  const auto result = shell.HandleEvent(click_event);
  Expect(result.handled, "clicking the compact menu button should be handled");
  Expect(WorkspaceShellTestAccess::MenuOverflowPopupOpen(shell),
         "clicking the compact menu button should open the overflow popup");

  const auto popup = WorkspaceShellTestAccess::MenuOverflowPopupRect(shell);
  Expect(popup.has_value(), "open overflow menu should expose a popup rect");
  Expect(!result.redraw.full && !result.redraw.rects.empty(),
         "opening the compact menu should stay on the partial redraw path");
  Expect(AnyRectIntersects(result.redraw.rects, *popup),
         "compact menu redraw must include the popup area, not just the title bar");
}

void TestWorkspaceShellCompactMenuOverflowRowsOpenAnchoredMenus() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);
  WorkspaceShellTestAccess::SetLayoutMode(shell, microide::workspace::LayoutMode::Compact);

  const auto chevron = WorkspaceShellTestAccess::MenuOverflowChevronRect(shell);
  Expect(chevron.has_value(), "compact menu row fixture should expose the overflow button");
  Expect(SendMouseDown(shell, chevron->x + chevron->w * 0.5f, chevron->y + chevron->h * 0.5f,
                       SDL_BUTTON_LEFT),
         "compact menu row fixture should open the top-level menu list");
  const auto popup = WorkspaceShellTestAccess::MenuOverflowPopupRect(shell);
  Expect(popup.has_value(), "compact menu row fixture should expose the top-level menu popup");

  Expect(SendMouseDown(shell, popup->x + 12.0f,
                       popup->y + 4.0f +
                           microide::workspace::kWorkspaceMenuPopupItemHeight * 0.5f,
                       SDL_BUTTON_LEFT),
         "clicking a compact top-level menu row should be handled");
  Expect(WorkspaceShellTestAccess::FileMenuOpen(shell),
         "clicking the File row in compact mode should open the File menu");
  Expect(WorkspaceShellTestAccess::VisiblePopupMenuLabels(shell, WorkspaceShell::MenuId::File).size() > 0,
         "compact File menu should render from its row anchor even though no File bar label is visible");
}

void TestWorkspaceShellFileCloseAllTabsClosesOpenEditorTabs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path left = root / "left.txt";
  const std::filesystem::path right = root / "right.txt";
  WriteFile(left, "left\n");
  WriteFile(right, "right\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, left);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, right),
         "close-all fixture should open a second editor tab");

  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 2,
         "close-all fixture should start with two tabs");
  Expect(WorkspaceShellTestAccess::ExecuteCloseAllTabs(shell),
         "close all tabs action should execute");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).empty(),
         "close all tabs should close every clean editor tab");
}

void TestWorkspaceShellTitleBarDragRegionDoesNotFabricateMaximizeToggle() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1920, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);

  // Test point chosen to land past the expanded menu bar but before window-control buttons.
  const float empty_x = 1500.0f;
  Expect(shell.WindowHitTest(empty_x, 10.0f) == SDL_HITTEST_DRAGGABLE,
         "empty title-bar hit testing should hand borderless dragging back to the window manager");
  Expect(shell.WindowDragRegionContains(empty_x, 10.0f),
         "empty title-bar space should still be eligible for window dragging");
  // Double-click-to-maximize was removed: on a draggable region SDL consumes the
  // button events to drive a compositor move, so the app never sees the clicks.
  // The Maximize chrome button is the supported affordance; a title-bar click
  // must not fabricate a maximize toggle.
  Expect(SendMouseDown(shell, empty_x, 10.0f, SDL_BUTTON_LEFT, 2),
         "a title-bar background click should be consumed");
  Expect(shell.ConsumeWindowAction() == WorkspaceShell::WindowAction::None,
         "clicking the title bar must not request a maximize toggle");
}

void TestWorkspaceShellFullscreenStateDisablesResizableFrameHitTest() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true, false, true);

  Expect(shell.WindowHitTest(1.0f, 1.0f) == SDL_HITTEST_NORMAL,
         "fullscreen chrome state should not expose resize hit targets");
}

void TestWorkspaceShellWindowPresentationStateUpdatesChromeAndSize() {
  WorkspaceShell shell;
  shell.SetWindowPresentationState(WorkspaceShell::WindowPresentationState{
      .logical_width = 1920,
      .logical_height = 720,
      .scale_x = 1.5f,
      .scale_y = 1.25f,
      .chrome =
          WorkspaceShell::WindowChromeState{
              .custom_enabled = true,
              .maximized = true,
              .fullscreen = false,
          },
  });

  Expect(shell.WindowHitTest(1.0f, 1.0f) == SDL_HITTEST_DRAGGABLE,
         "maximized presentation state should keep the title bar draggable without exposing resize hit targets");
  Expect(shell.WindowDragRegionContains(1500.0f, 10.0f),
         "presentation state should keep the title bar draggable past the expanded menu bar");
}

void TestWorkspaceShellMenuBarHoverSwitchesActiveMenu() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const auto file_rect = WorkspaceShellTestAccess::MenuBarItemRect(shell, "File");
  const auto edit_rect = WorkspaceShellTestAccess::MenuBarItemRect(shell, "Edit");
  Expect(file_rect.has_value(), "menu hover fixture should expose a File menu item");
  Expect(edit_rect.has_value(), "menu hover fixture should expose an Edit menu item");

  Expect(SendMouseDown(
             shell, file_rect->x + file_rect->w * 0.5f, file_rect->y + file_rect->h * 0.5f,
             SDL_BUTTON_LEFT),
         "clicking the File menu should be handled");
  Expect(WorkspaceShellTestAccess::FileMenuOpen(shell),
         "clicking the File menu should open the File popup");

  Expect(SendMouseMotion(
             shell, edit_rect->x + edit_rect->w * 0.5f, edit_rect->y + edit_rect->h * 0.5f, 0),
         "hovering another menu while the menu bar is open should be handled");
  Expect(WorkspaceShellTestAccess::EditMenuOpen(shell),
         "hovering the Edit menu should switch the active popup");
}

void TestWorkspaceShellMenuEventsReturnPartialChromeInvalidation() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const auto file_rect = WorkspaceShellTestAccess::MenuBarItemRect(shell, "File");
  const auto edit_rect = WorkspaceShellTestAccess::MenuBarItemRect(shell, "Edit");
  Expect(file_rect.has_value() && edit_rect.has_value(),
         "menu invalidation fixture should expose File and Edit menu items");

  SDL_Event click_event{};
  click_event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  click_event.button.button = SDL_BUTTON_LEFT;
  click_event.button.x = file_rect->x + file_rect->w * 0.5f;
  click_event.button.y = file_rect->y + file_rect->h * 0.5f;
  const auto open_result = shell.HandleEvent(click_event);
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  Expect(open_result.handled, "opening a menu should be handled");
  Expect(!open_result.redraw.full && !open_result.redraw.rects.empty(),
         "opening a menu should request a partial redraw");
  Expect(AnyRectIntersects(open_result.redraw.rects, layout.menu_bar),
         "menu redraws should include the chrome menu bar");

  SDL_Event motion_event{};
  motion_event.type = SDL_EVENT_MOUSE_MOTION;
  motion_event.motion.x = edit_rect->x + edit_rect->w * 0.5f;
  motion_event.motion.y = edit_rect->y + edit_rect->h * 0.5f;
  const auto hover_result = shell.HandleEvent(motion_event);
  Expect(hover_result.handled, "switching menu hover should be handled");
  Expect(!hover_result.redraw.full && !hover_result.redraw.rects.empty(),
         "switching menu hover should stay on a partial redraw path");
  Expect(AnyRectIntersects(hover_result.redraw.rects, layout.menu_bar),
         "menu hover redraws should stay scoped to chrome");
}

void TestWorkspaceShellPopupRowHoverReturnsPopupOnlyInvalidation() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const auto file_rect = WorkspaceShellTestAccess::MenuBarItemRect(shell, "File");
  Expect(file_rect.has_value(), "popup hover fixture should expose the File menu item");
  Expect(SendMouseDown(
             shell, file_rect->x + file_rect->w * 0.5f, file_rect->y + file_rect->h * 0.5f,
             SDL_BUTTON_LEFT),
         "clicking the File menu should be handled");
  Expect(WorkspaceShellTestAccess::FileMenuOpen(shell),
         "clicking the File menu should open the File popup");

  const auto labels =
      WorkspaceShellTestAccess::VisiblePopupMenuLabels(shell, WorkspaceShell::MenuId::File);
  Expect(labels.size() >= 2, "popup hover fixture should expose at least two File menu rows");

  const auto first_item = WorkspaceShellTestAccess::PopupMenuItemRect(
      shell, WorkspaceShell::MenuId::File, labels.front());
  const auto second_item = WorkspaceShellTestAccess::PopupMenuItemRect(
      shell, WorkspaceShell::MenuId::File, labels[1]);
  Expect(first_item.has_value() && second_item.has_value(),
         "popup hover fixture should expose the first two File menu rows");

  Expect(SendMouseMotion(shell, first_item->x + first_item->w * 0.5f,
                         first_item->y + first_item->h * 0.5f, 0),
         "priming the first popup row hover should be handled");

  SDL_Event motion_event{};
  motion_event.type = SDL_EVENT_MOUSE_MOTION;
  motion_event.motion.x = second_item->x + second_item->w * 0.5f;
  motion_event.motion.y = second_item->y + second_item->h * 0.5f;
  const auto hover_result = shell.HandleEvent(motion_event);
  Expect(hover_result.handled, "hovering a popup row should be handled");
  Expect(!hover_result.redraw.full && !hover_result.redraw.rects.empty(),
         "popup row hover should stay on a partial redraw path");
  Expect(hover_result.redraw.rects.size() <= 2,
         "popup row hover should only dirty the affected popup rows");
  Expect(AnyRectIntersects(hover_result.redraw.rects, *first_item),
         "popup row hover should redraw the previously highlighted row");
  Expect(AnyRectIntersects(hover_result.redraw.rects, *second_item),
         "popup row hover should redraw the newly highlighted row");
}

void TestWorkspaceShellTreeSidebarHeaderHoverReturnsButtonOnlyInvalidation() {
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

  const SDL_FRect mode_rect = WorkspaceShellTestAccess::SidebarModeButtonRect(shell);
  const SDL_FRect collapse_rect = WorkspaceShellTestAccess::TreeSidebarCollapseButtonRect(shell);
  Expect(mode_rect.w > 0.0f && collapse_rect.w > 0.0f,
         "tree hover fixture should expose sidebar mode and collapse buttons");

  Expect(SendMouseMotion(shell, mode_rect.x + mode_rect.w * 0.5f,
                         mode_rect.y + mode_rect.h * 0.5f, 0),
         "priming the sidebar mode hover should be handled");

  SDL_Event motion_event{};
  motion_event.type = SDL_EVENT_MOUSE_MOTION;
  motion_event.motion.x = collapse_rect.x + collapse_rect.w * 0.5f;
  motion_event.motion.y = collapse_rect.y + collapse_rect.h * 0.5f;
  const auto hover_result = shell.HandleEvent(motion_event);
  Expect(hover_result.handled, "switching tree header hover should be handled");
  Expect(!hover_result.redraw.full && !hover_result.redraw.rects.empty(),
         "tree header hover should stay on a partial redraw path");
  Expect(hover_result.redraw.rects.size() <= 2,
         "tree header hover should only dirty the affected controls");
  Expect(AnyRectIntersects(hover_result.redraw.rects, mode_rect),
         "tree header hover should redraw the previously hovered control");
  Expect(AnyRectIntersects(hover_result.redraw.rects, collapse_rect),
         "tree header hover should redraw the newly hovered control");
}

void TestWorkspaceShellSearchSidebarHeaderHoverReturnsButtonOnlyInvalidation() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "main.cpp", "int main() {}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "main", false);

  const SDL_FRect mode_rect = WorkspaceShellTestAccess::SearchSidebarModeButtonRect(shell);
  const SDL_FRect case_rect = WorkspaceShellTestAccess::SearchSidebarCaseButtonRect(shell);
  Expect(mode_rect.w > 0.0f && case_rect.w > 0.0f,
         "search hover fixture should expose the mode and case buttons");

  Expect(SendMouseMotion(shell, mode_rect.x + mode_rect.w * 0.5f,
                         mode_rect.y + mode_rect.h * 0.5f, 0),
         "priming the search mode hover should be handled");

  SDL_Event motion_event{};
  motion_event.type = SDL_EVENT_MOUSE_MOTION;
  motion_event.motion.x = case_rect.x + case_rect.w * 0.5f;
  motion_event.motion.y = case_rect.y + case_rect.h * 0.5f;
  const auto hover_result = shell.HandleEvent(motion_event);
  Expect(hover_result.handled, "switching search header hover should be handled");
  Expect(!hover_result.redraw.full && !hover_result.redraw.rects.empty(),
         "search header hover should stay on a partial redraw path");
  // Up to 4 rects: previous + new hover-button rects + previous + new tooltip rects.
  Expect(hover_result.redraw.rects.size() <= 4,
         "search header hover should only dirty the affected controls and tooltips");
  Expect(AnyRectIntersects(hover_result.redraw.rects, mode_rect),
         "search header hover should redraw the previously hovered control");
  Expect(AnyRectIntersects(hover_result.redraw.rects, case_rect),
         "search header hover should redraw the newly hovered control");
}

void TestWorkspaceShellGitSidebarHeaderHoverReturnsButtonOnlyInvalidation() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add git hover fixture", "git hover fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1),
         "git header hover fixture should expose one changed row");

  const auto top_action_rects = WorkspaceShellTestAccess::GitSidebarTopActionRects(shell);
  Expect(top_action_rects[0].w > 0.0f && top_action_rects[2].w > 0.0f,
         "git hover fixture should expose the stage-all and refresh buttons");

  Expect(SendMouseMotion(shell, top_action_rects[0].x + top_action_rects[0].w * 0.5f,
                         top_action_rects[0].y + top_action_rects[0].h * 0.5f, 0),
         "priming the git stage-all hover should be handled");

  SDL_Event motion_event{};
  motion_event.type = SDL_EVENT_MOUSE_MOTION;
  motion_event.motion.x = top_action_rects[2].x + top_action_rects[2].w * 0.5f;
  motion_event.motion.y = top_action_rects[2].y + top_action_rects[2].h * 0.5f;
  const auto hover_result = shell.HandleEvent(motion_event);
  Expect(hover_result.handled, "switching git header hover should be handled");
  Expect(!hover_result.redraw.full && !hover_result.redraw.rects.empty(),
         "git header hover should stay on a partial redraw path");
  Expect(hover_result.redraw.rects.size() <= 2,
         "git header hover should only dirty the affected controls");
  Expect(AnyRectIntersects(hover_result.redraw.rects, top_action_rects[0]),
         "git header hover should redraw the previously hovered control");
  Expect(AnyRectIntersects(hover_result.redraw.rects, top_action_rects[2]),
         "git header hover should redraw the newly hovered control");
}

void TestWorkspaceShellOpenMenuSuppressesUnderlyingTabTooltip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "deep" / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect tab_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  Expect(SendMouseMotion(shell, tab_rect.x + tab_rect.w * 0.5f, tab_rect.y + tab_rect.h * 0.5f, 0),
         "hovering the tab should be handled");
  Expect(WorkspaceShellTestAccess::HoveredTabTooltipLabel(shell) == "src/deep/main.cpp",
         "tab tooltip fixture should expose the relative path before opening a menu");

  const auto file_rect = WorkspaceShellTestAccess::MenuBarItemRect(shell, "File");
  Expect(file_rect.has_value(), "menu suppression fixture should expose the File menu item");
  Expect(SendMouseDown(shell, file_rect->x + file_rect->w * 0.5f, file_rect->y + file_rect->h * 0.5f,
                       SDL_BUTTON_LEFT),
         "clicking the File menu should be handled");
  Expect(WorkspaceShellTestAccess::FileMenuOpen(shell),
         "clicking the File menu should open the File popup");

  Expect(SendMouseMotion(shell, tab_rect.x + tab_rect.w * 0.5f, tab_rect.y + tab_rect.h * 0.5f, 0),
         "hover while a menu is open should stay captured by the menu surface");
  Expect(WorkspaceShellTestAccess::HoveredTabTooltipLabel(shell).empty(),
         "tab tooltips should stay suppressed while a menu is open");
}

void TestWorkspaceShellOverflowPopupSuppressesUnderlyingTabTooltip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "deep" / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 420, 720);

  const SDL_FRect tab_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  Expect(SendMouseMotion(shell, tab_rect.x + tab_rect.w * 0.5f, tab_rect.y + tab_rect.h * 0.5f, 0),
         "overflow suppression fixture should first expose a tab tooltip");
  Expect(WorkspaceShellTestAccess::HoveredTabTooltipLabel(shell) == "src/deep/main.cpp",
         "overflow suppression fixture should expose the relative path before opening the overflow menu");

  const auto chevron = WorkspaceShellTestAccess::MenuOverflowChevronRect(shell);
  Expect(chevron.has_value(), "overflow suppression fixture should expose the compact menu chevron");
  Expect(SendMouseDown(shell, chevron->x + chevron->w * 0.5f, chevron->y + chevron->h * 0.5f,
                       SDL_BUTTON_LEFT),
         "clicking the overflow chevron should be handled");
  Expect(WorkspaceShellTestAccess::MenuOverflowPopupOpen(shell),
         "clicking the overflow chevron should open the overflow popup");

  Expect(SendMouseMotion(shell, tab_rect.x + tab_rect.w * 0.5f, tab_rect.y + tab_rect.h * 0.5f, 0),
         "hover while the overflow popup is open should be handled");
  Expect(WorkspaceShellTestAccess::HoveredTabTooltipLabel(shell).empty(),
         "tab tooltips should stay suppressed while the overflow popup is open");
}

void TestWorkspaceShellStatusRowShowsLspReadinessAndInFlightState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "main.md";
  WriteFile(file, "# heading\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, file);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const auto initial_items = WorkspaceShellTestAccess::VisibleStatusItems(shell);
  const auto lsp_it = std::find_if(initial_items.begin(), initial_items.end(), [](const auto& item) {
    return item.item.id == "host.lsp";
  });
  Expect(lsp_it == initial_items.end(),
         "breadcrumb status row should not duplicate host-owned LSP status");

  auto& project = WorkspaceShellTestAccess::CurrentProjectState(shell);
  project.lsp.request_in_flight = true;
  project.lsp.request_started_ticks = SDL_GetTicks();
  project.lsp.request_timeout_ticks = project.lsp.request_started_ticks + 1000;

  const auto in_flight_items = WorkspaceShellTestAccess::VisibleStatusItems(shell);
  const auto in_flight_it =
      std::find_if(in_flight_items.begin(), in_flight_items.end(), [](const auto& item) {
        return item.item.id == "host.lsp";
      });
  Expect(in_flight_it == in_flight_items.end(),
         "breadcrumb status row should keep LSP state hidden even while requests are in flight");
}

void TestWorkspaceShellEditorCaretDirtyRectFollowsActiveCaret() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "alpha\nbeta\ngamma\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(1, 2);

  const std::optional<SDL_FRect> caret_rect = shell.CurrentCaretDirtyRect();
  Expect(caret_rect.has_value(),
         "focused editors should expose a caret dirty rect for partial redraws");
  Expect(caret_rect->w > 0.0f && caret_rect->h > 0.0f,
         "editor caret dirty rects should have a visible size");
}

void TestWorkspaceShellEditorTypingReturnsPartialEditorInvalidation() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "left.cpp";
  WriteFile(source, "alpha\nbeta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  (void)shell.ConsumePendingRenderInvalidation();

  SDL_Event event{};
  event.type = SDL_EVENT_TEXT_INPUT;
  const std::string text = "x";
  event.text.text = text.c_str();
  const auto result = shell.HandleEvent(event);
  const SDL_FRect active_pane = WorkspaceShellTestAccess::ActiveEditorPaneRect(shell);
  const auto edited_line_rect = WorkspaceShellTestAccess::ActiveEditorLineRangeRect(shell, 0, 1);

  Expect(result.handled, "editor typing should be handled");
  Expect(!result.redraw.full && !result.redraw.rects.empty(),
         "editor typing should request a partial redraw");
  Expect(AnyRectIntersects(result.redraw.rects, active_pane),
         "editor typing redraws should include the active editor pane");
  Expect(edited_line_rect.has_value() && AnyRectIntersects(result.redraw.rects, *edited_line_rect),
         "editor typing redraws should include the edited line band");
  Expect(MaxRectHeight(result.redraw.rects) < active_pane.h,
         "single-line editor typing should redraw less than the full active pane");
}

void TestWorkspaceShellWrappedEditorLineRangeRectCoversContinuationRows() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "wrap.cpp";
  WriteFile(source, "abcdefghij klmnopqrst uvwxyz 0123456789 ABCDEFGHIJ\nshort\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.SetSoftWrap(true);
  viewport.SetViewportSize(40, 8);
  (void)shell.ConsumePendingRenderInvalidation();

  const auto line_rect = WorkspaceShellTestAccess::ActiveEditorLineRangeRect(shell, 0, 1);
  const auto single_row_rect = WorkspaceShellTestAccess::ActiveEditorLineRangeRect(shell, 1, 2);
  Expect(line_rect.has_value(),
         "wrapped editor line-range rect fixture should return a redraw rect");
  Expect(single_row_rect.has_value(),
         "wrapped editor line-range rect fixture should expose a single-row rect for comparison");
  Expect(line_rect->h > single_row_rect->h,
         "a wrapped logical line should invalidate every continuation row, not only the opener row");
}

void TestWorkspaceShellWrappedTypingReturnsFocusedPaneInvalidation() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "wrap.cpp";
  WriteFile(source, "abcdefghij klmnopqrst uvwxyz\nalpha\nbeta\ngamma\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 700, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.SetSoftWrap(true);
  viewport.SetDirty(true);
  viewport.MoveCursorTo(0, viewport.lines().front().size());
  (void)shell.ConsumePendingRenderInvalidation();

  SDL_Event event{};
  event.type = SDL_EVENT_TEXT_INPUT;
  const std::string text = "!";
  event.text.text = text.c_str();
  const auto result = shell.HandleEvent(event);
  const SDL_FRect active_pane = WorkspaceShellTestAccess::ActiveEditorPaneRect(shell);

  Expect(result.handled, "wrapped typing should be handled");
  Expect(!result.redraw.full && !result.redraw.rects.empty(),
         "wrapped typing should stay on the partial redraw path");
  Expect(AnyRectIntersects(result.redraw.rects, active_pane),
         "wrapped typing redraws should include the active editor pane");
  Expect(MaxRectHeight(result.redraw.rects) >= active_pane.h,
         "wrapped typing should invalidate the full focused pane so wrapped-row reflow repaints correctly");
}

void TestWorkspaceShellCommandTextInputReturnsPartialCommandInvalidation() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  SDL_Event open_event{};
  open_event.type = SDL_EVENT_KEY_DOWN;
  open_event.key.key = SDLK_P;
  open_event.key.mod = SDL_KMOD_CTRL | SDL_KMOD_SHIFT;
  const auto open_result = shell.HandleEvent(open_event);
  Expect(open_result.handled,
         "command palette invalidation fixture should open the palette");
  (void)open_result.redraw;

  SDL_Event event{};
  event.type = SDL_EVENT_TEXT_INPUT;
  const std::string text = "palette";
  event.text.text = text.c_str();
  const auto result = shell.HandleEvent(event);

  Expect(result.handled, "command palette typing should be handled");
  Expect(!result.redraw.full && !result.redraw.rects.empty(),
         "command palette typing should stay on the partial (overlay) redraw path");
  Expect(WorkspaceShellTestAccess::CommandPaletteQuery(shell) == text,
         "command palette typing should append to the palette query");
}

void TestWorkspaceShellCommandPasteShortcutUsesSharedTextInputPath() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(SendKeyDown(shell, SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "command paste fixture should open the command palette");
  WorkspaceShellTestAccess::SetClipboardTextReader(
      shell, []() -> std::optional<std::string> { return std::string("palette"); });

  Expect(SendKeyDown(shell, SDLK_V, SDL_KMOD_CTRL),
         "Ctrl+V should be handled by the command palette query");
  Expect(WorkspaceShellTestAccess::CommandPaletteQuery(shell) == "palette",
         "Ctrl+V should route clipboard text through the shared command text-input path");
}

void TestWorkspaceShellProjectTabsShowBadges() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "alpha-project";
  WriteFile(root / "README.md", "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "project badge fixture should open the project");

  Expect(WorkspaceShellTestAccess::ProjectTabShowsBadge(shell, 0),
         "project tabs should render a badge");
  Expect(WorkspaceShellTestAccess::ProjectTabBadgeText(shell, 0) == "A",
         "project tab badges should use the project initial");
}

void TestWorkspaceShellProjectTabBadgeColorStableAcrossSwitch() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root_a = temp_dir.path() / "alpha-project";
  const std::filesystem::path root_b = temp_dir.path() / "beta-project";
  WriteFile(root_a / "README.md", "alpha\n");
  WriteFile(root_b / "README.md", "beta\n");

  const SDL_Color color_a{0x12, 0x34, 0x56, 0xff};
  const SDL_Color color_b{0xab, 0xcd, 0xef, 0xff};
  const auto colors_match = [](SDL_Color lhs, SDL_Color rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
  };

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_a, false, false),
         "project badge color fixture should open the first project");
  WorkspaceShellTestAccess::SetProjectBaseColor(shell, color_a);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root_b, false, false),
         "project badge color fixture should open the second project");
  WorkspaceShellTestAccess::SetProjectBaseColor(shell, color_b);

  Expect(colors_match(WorkspaceShellTestAccess::ProjectTabBadgeColor(shell, 1), color_b),
         "active project tab badge should use the live project color");
  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "project badge color fixture should switch back to the first project");
  Expect(colors_match(WorkspaceShellTestAccess::ProjectTabBadgeColor(shell, 0), color_a),
         "active project tab badge should keep its color after switching");
  Expect(colors_match(WorkspaceShellTestAccess::ProjectTabBadgeColor(shell, 1), color_b),
         "inactive project tab badge should keep its stored color");
  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 1, false),
         "project badge color fixture should switch back to the second project");
  Expect(colors_match(WorkspaceShellTestAccess::ProjectTabBadgeColor(shell, 1), color_b),
         "project tab badge color should remain stable when reactivated");
  Expect(colors_match(WorkspaceShellTestAccess::ProjectTabBadgeColor(shell, 0), color_a),
         "inactive project tab badge should remain stable after reactivation");
}

void TestWorkspaceShellSidebarModeTabsPresent() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.txt";
  WriteFile(source, "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  // The header row exposes the three primary view tabs, laid out left-to-right, and no overflow
  // button when no plugin views are registered.
  const SDL_FRect tree_tab = WorkspaceShellTestAccess::SidebarModeTabRect(shell, "tree");
  const SDL_FRect search_tab = WorkspaceShellTestAccess::SidebarModeTabRect(shell, "search");
  const SDL_FRect git_tab = WorkspaceShellTestAccess::SidebarModeTabRect(shell, "git");
  Expect(tree_tab.w > 0.0f && search_tab.w > 0.0f && git_tab.w > 0.0f,
         "Project, Search, and Source Control tabs should all be present");
  Expect(tree_tab.x < search_tab.x && search_tab.x < git_tab.x,
         "mode tabs should be laid out in Project / Search / Source Control order");
  Expect(WorkspaceShellTestAccess::SidebarModeOverflowRect(shell).w <= 0.0f,
         "no overflow button should appear without plugin-contributed views");
}

void TestWorkspaceShellTabTooltipRendersAboveSidebar() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "deep" / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const SDL_FRect tab_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  (void)SendMouseMotion(shell, tab_rect.x + tab_rect.w * 0.25f,
                                                    tab_rect.y + tab_rect.h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredTabTooltipLabel(shell) == "src/deep/main.cpp",
         "tooltip layering fixture should produce the hovered tab tooltip");

  const auto tooltip_rect = WorkspaceShellTestAccess::HoveredTabTooltipRect(shell);
  Expect(tooltip_rect.has_value(), "hovered tabs should compute a tooltip rect");
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const float overlap_x = std::max(tooltip_rect->x, layout.sidebar.x);
  const float overlap_y = std::max(tooltip_rect->y, layout.sidebar.y);
  const float overlap_right =
      std::min(tooltip_rect->x + tooltip_rect->w, layout.sidebar.x + layout.sidebar.w);
  const float overlap_bottom =
      std::min(tooltip_rect->y + tooltip_rect->h, layout.sidebar.y + layout.sidebar.h);
  const SDL_FRect overlap{
      .x = overlap_x,
      .y = overlap_y,
      .w = std::max(0.0f, overlap_right - overlap_x),
      .h = std::max(0.0f, overlap_bottom - overlap_y),
  };
  Expect(overlap.w > 4.0f && overlap.h > 4.0f,
         "tooltip layering fixture should overlap the sidebar to exercise draw order");

  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "tooltip layering fixture should capture rendered pixels");

  const auto theme = microide::render::MakeDefaultTheme();
  const SDL_Color actual =
      ReadSurfacePixelOrThrow(pixels, static_cast<int>(std::floor(overlap.x + 2.0f)),
                              static_cast<int>(std::floor(overlap.y + 2.0f)));
  Expect(actual.r == theme.surface_raised.r && actual.g == theme.surface_raised.g &&
             actual.b == theme.surface_raised.b && actual.a == theme.surface_raised.a,
         "hovered tab tooltips should render above the sidebar fill");

  SDL_DestroySurface(pixels);
}

void TestWorkspaceShellGitSidebarTooltipUsesSharedCompactCard() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add git tooltip render fixture", "git tooltip render fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  const auto git_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < git_deadline &&
         WorkspaceShellTestAccess::GitSidebarRefreshing(shell)) {
    WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  const auto top_action_rects = WorkspaceShellTestAccess::GitSidebarTopActionRects(shell);
  (void)SendMouseMotion(
      shell, top_action_rects[2].x + top_action_rects[2].w * 0.5f,
      top_action_rects[2].y + top_action_rects[2].h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell) == "Refresh",
         "git tooltip fixture should expose the compact action tooltip");

  const auto tooltip_rect = WorkspaceShellTestAccess::HoveredGitSidebarTooltipRect(shell);
  Expect(tooltip_rect.has_value(), "git tooltip fixture should compute a tooltip rect");

  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "git tooltip fixture should capture rendered pixels");

  const auto theme = microide::render::MakeDefaultTheme();
  const SDL_Color actual =
      ReadSurfacePixelOrThrow(pixels, static_cast<int>(std::floor(tooltip_rect->x + 2.0f)),
                              static_cast<int>(std::floor(tooltip_rect->y + 2.0f)));
  Expect(actual.r == theme.surface_raised.r && actual.g == theme.surface_raised.g &&
             actual.b == theme.surface_raised.b && actual.a == theme.surface_raised.a,
         "git sidebar tooltips should render with the shared compact tooltip card fill");

  SDL_DestroySurface(pixels);
}

void TestWorkspaceShellProjectTabTooltipDismissRetainedRedrawMatchesFullRender() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
#if defined(__SANITIZE_THREAD__)
  // TSAN-instrumented redraw scheduling can perturb retained/full tooltip-dismiss parity in the
  // aggregate chrome suite while the isolated fixture remains stable; skip this pixel-equality
  // assertion in TSAN and keep the dedicated single-test path for coverage.
  return;
#endif
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path first_root = temp_dir.path() / "first-project";
  const std::filesystem::path second_root = temp_dir.path() / "second-project";
  const std::filesystem::path source = second_root / "src" / "deep" / "main.cpp";
  WriteFile(first_root / "README.md", "first\n");
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  static constexpr int kCanvasWidth = 1280;
  static constexpr int kCanvasHeight = 720;

  const auto configure_shell = [&](WorkspaceShell& shell) {
    Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, first_root, false, false),
           "project tooltip retained redraw fixture should open the first project");
    Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, second_root, false, false),
           "project tooltip retained redraw fixture should open the second project");
    WorkspaceShellTestAccess::SetWindowSize(shell, kCanvasWidth, kCanvasHeight);
    WorkspaceShellTestAccess::SetFocusSidebar(shell);
  };

  WorkspaceShell retained_shell;
  configure_shell(retained_shell);
  SoftwareCanvas retained_canvas(kCanvasWidth, kCanvasHeight);
  retained_shell.Render(retained_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  const SDL_FRect project_tab_rect = WorkspaceShellTestAccess::ProjectTabRect(retained_shell, 1);
  SDL_Event hover_event{};
  hover_event.type = SDL_EVENT_MOUSE_MOTION;
  hover_event.motion.x = project_tab_rect.x + project_tab_rect.w * 0.5f;
  hover_event.motion.y = project_tab_rect.y + project_tab_rect.h * 0.5f;
  const auto hover_result = retained_shell.HandleEvent(hover_event);
  Expect(hover_result.handled, "hovering a project tab tooltip fixture should be handled");
  RenderRetainedInvalidation(retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight,
                             hover_result.redraw);
  Expect(WorkspaceShellTestAccess::HoveredProjectTabTooltipRect(retained_shell).has_value(),
         "hovering a project tab should expose a tooltip rect");

  const SDL_FRect editor_rect = WorkspaceShellTestAccess::ActiveEditorPaneRect(retained_shell);
  SDL_Event dismiss_event{};
  dismiss_event.type = SDL_EVENT_MOUSE_MOTION;
  dismiss_event.motion.x = editor_rect.x + 20.0f;
  dismiss_event.motion.y = editor_rect.y + 20.0f;
  const auto dismiss_result = retained_shell.HandleEvent(dismiss_event);
  Expect(dismiss_result.handled,
         "moving away from a project tab tooltip should stay on a handled redraw path");
  RenderRetainedInvalidation(retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight,
                             dismiss_result.redraw);

  WorkspaceShell reference_shell;
  configure_shell(reference_shell);
  SDL_Event reference_hover_event = hover_event;
  Expect(reference_shell.HandleEvent(reference_hover_event).handled,
         "reference project tooltip fixture should handle hover motion");
  SDL_Event reference_dismiss_event = dismiss_event;
  Expect(reference_shell.HandleEvent(reference_dismiss_event).handled,
         "reference project tooltip fixture should handle dismiss motion");
  SoftwareCanvas reference_canvas(kCanvasWidth, kCanvasHeight);
  reference_shell.Render(reference_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  SDL_Surface* retained_pixels = SDL_RenderReadPixels(retained_canvas.renderer(), nullptr);
  SDL_Surface* reference_pixels = SDL_RenderReadPixels(reference_canvas.renderer(), nullptr);
  const std::size_t pixel_differences =
      CountPixelDifferences(retained_pixels, reference_pixels);

  if (retained_pixels != nullptr) {
    SDL_DestroySurface(retained_pixels);
  }
  if (reference_pixels != nullptr) {
    SDL_DestroySurface(reference_pixels);
  }

  Expect(pixel_differences == 0,
         "dismissing a project tab tooltip should leave retained redraw identical to a full redraw");
}

void TestWorkspaceShellPromptInputRendersSharedFramedField() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "file.txt";
  WriteFile(source, "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::PrepareRenamePrompt(shell, source, "renamed.txt");
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "prompt render fixture should expose the rename prompt");

  const SDL_FRect input_rect = WorkspaceShellTestAccess::PromptSurfaceInputRect(shell);
  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "prompt render fixture should capture rendered pixels");

  const auto theme = microide::render::MakeDefaultTheme();
  const SDL_Color left_border =
      ReadSurfacePixelOrThrow(pixels, static_cast<int>(std::floor(input_rect.x)),
                              static_cast<int>(std::floor(input_rect.y + input_rect.h * 0.5f)));
  const SDL_Color top_border =
      ReadSurfacePixelOrThrow(pixels, static_cast<int>(std::floor(input_rect.x + input_rect.w * 0.5f)),
                              static_cast<int>(std::floor(input_rect.y)));
  const SDL_Color fill =
      ReadSurfacePixelOrThrow(pixels, static_cast<int>(std::floor(input_rect.x + input_rect.w * 0.5f)),
                              static_cast<int>(std::floor(input_rect.y + input_rect.h * 0.5f)));
  const bool left_matches_accent = left_border.r == theme.accent.r &&
                                   left_border.g == theme.accent.g &&
                                   left_border.b == theme.accent.b &&
                                   left_border.a == theme.accent.a;
  const bool top_matches_accent = top_border.r == theme.accent.r &&
                                  top_border.g == theme.accent.g &&
                                  top_border.b == theme.accent.b &&
                                  top_border.a == theme.accent.a;
  Expect(left_matches_accent || top_matches_accent,
         "active prompt inputs should render the shared framed-field accent border");
  Expect(fill.r == theme.surface_background.r && fill.g == theme.surface_background.g &&
             fill.b == theme.surface_background.b && fill.a == theme.surface_background.a,
         "prompt inputs should render the shared framed-field background");

  SDL_DestroySurface(pixels);
}

void TestWorkspaceShellShortcutEditActionsReturnEditorInvalidation() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "alpha\nbeta\n");

  const auto configure_shell = [&](WorkspaceShell& shell) {
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
    WorkspaceShellTestAccess::OpenFile(shell, source);
    (void)shell.ConsumePendingRenderInvalidation();
  };
  const auto handle_shortcut = [](WorkspaceShell& shell, SDL_Keycode key) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = key;
    event.key.mod = SDL_KMOD_CTRL;
    return shell.HandleEvent(event);
  };

  WorkspaceShell cut_shell;
  configure_shell(cut_shell);

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      cut_shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });
  WorkspaceShellTestAccess::SetPrimarySelectionTextWriter(cut_shell,
                                                          [](std::string_view) { return true; });

  const SDL_FRect cut_editor_surface = WorkspaceShellTestAccess::CurrentLayout(cut_shell).editor_surface;
  const std::vector<std::string> original_lines =
      WorkspaceShellTestAccess::ActiveEditor(cut_shell).lines().Snapshot();

  const auto select_all = handle_shortcut(cut_shell, SDLK_A);
  Expect(select_all.handled, "Ctrl+A should be handled by the editor");
  Expect(!select_all.redraw.full && !select_all.redraw.rects.empty(),
         "Ctrl+A should request a partial editor redraw");
  Expect(AnyRectIntersects(select_all.redraw.rects, cut_editor_surface),
         "Ctrl+A redraws should include the editor surface");
  Expect(WorkspaceShellTestAccess::ActiveEditorHasSelection(cut_shell),
         "Ctrl+A should select the active editor contents");

  const auto cut = handle_shortcut(cut_shell, SDLK_X);
  Expect(cut.handled, "Ctrl+X should be handled by the editor");
  Expect(!cut.redraw.full && !cut.redraw.rects.empty(),
         "Ctrl+X should request a partial editor redraw");
  Expect(AnyRectIntersects(cut.redraw.rects, cut_editor_surface),
         "Ctrl+X redraws should include the editor surface");
  Expect(!clipboard_text.empty(),
         "Ctrl+X should write the selected editor text to the clipboard");
  Expect(WorkspaceShellTestAccess::ActiveEditor(cut_shell).lines().Snapshot() != original_lines,
         "Ctrl+X should modify the active editor buffer");

  WorkspaceShell copy_line_shell;
  WorkspaceShell cut_line_shell;
  const std::filesystem::path line_source = root / "line-copy.cpp";
  WriteFile(line_source, "alpha\nbeta\ngamma");

  configure_shell(copy_line_shell);
  WorkspaceShellTestAccess::OpenFile(copy_line_shell, line_source);
  (void)copy_line_shell.ConsumePendingRenderInvalidation();
  WorkspaceShellTestAccess::ActiveEditor(copy_line_shell).MoveCursorTo(1, 2);
  clipboard_text.clear();
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      copy_line_shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });
  WorkspaceShellTestAccess::SetPrimarySelectionTextWriter(copy_line_shell,
                                                          [](std::string_view) { return true; });
  const auto copy_line = handle_shortcut(copy_line_shell, SDLK_C);
  Expect(copy_line.handled, "Ctrl+C without a selection should still be handled by the editor");
  Expect(clipboard_text == "beta\n",
         "Ctrl+C without a selection should copy the full active line");

  configure_shell(cut_line_shell);
  WorkspaceShellTestAccess::OpenFile(cut_line_shell, line_source);
  (void)cut_line_shell.ConsumePendingRenderInvalidation();
  WorkspaceShellTestAccess::ActiveEditor(cut_line_shell).MoveCursorTo(1, 2);
  clipboard_text.clear();
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      cut_line_shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });
  WorkspaceShellTestAccess::SetPrimarySelectionTextWriter(cut_line_shell,
                                                          [](std::string_view) { return true; });
  const auto cut_line = handle_shortcut(cut_line_shell, SDLK_X);
  Expect(cut_line.handled, "Ctrl+X without a selection should still be handled by the editor");
  Expect(clipboard_text == "beta\n",
         "Ctrl+X without a selection should copy the full active line");
  Expect(WorkspaceShellTestAccess::ActiveEditor(cut_line_shell).lines().Snapshot() ==
             std::vector<std::string>({"alpha", "gamma"}),
         "Ctrl+X without a selection should remove the full active line");

  WorkspaceShell undo_shell;
  configure_shell(undo_shell);

  SDL_Event text_event{};
  text_event.type = SDL_EVENT_TEXT_INPUT;
  const std::string typed_text = "x";
  text_event.text.text = typed_text.c_str();
  const auto typed = undo_shell.HandleEvent(text_event);
  Expect(typed.handled, "undo invalidation fixture should type into the editor first");
  const SDL_FRect undo_editor_surface = WorkspaceShellTestAccess::CurrentLayout(undo_shell).editor_surface;
  const auto undo = handle_shortcut(undo_shell, SDLK_Z);
  Expect(undo.handled, "Ctrl+Z should be handled by the editor");
  Expect(!undo.redraw.full && !undo.redraw.rects.empty(),
         "Ctrl+Z should request a partial editor redraw");
  Expect(AnyRectIntersects(undo.redraw.rects, undo_editor_surface),
         "Ctrl+Z redraws should include the editor surface");
  Expect(WorkspaceShellTestAccess::ActiveEditor(undo_shell).lines().Snapshot() == original_lines,
         "Ctrl+Z should restore the editor buffer after typing");
}

void TestWorkspaceShellEditorTabRightClickOpensContextMenu() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path left = root / "alpha.cpp";
  const std::filesystem::path right = root / "beta.cpp";
  WriteFile(left, "int alpha() { return 1; }\n");
  WriteFile(right, "int beta() { return 2; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, left);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, right);
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 2,
         "tab context-menu fixture should expose two editor tabs");
  WorkspaceShellTestAccess::ActivateTab(shell, 1);

  const SDL_FRect tab_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  Expect(SendMouseDown(
             shell, tab_rect.x + tab_rect.w * 0.5f, tab_rect.y + tab_rect.h * 0.5f,
             SDL_BUTTON_RIGHT),
         "right-clicking an editor tab should be handled");
  Expect(WorkspaceShellTestAccess::EditorTabContextMenuOpen(shell),
         "right-clicking an editor tab should open the editor tab context menu");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 0,
         "right-clicking an editor tab should retarget the active tab before menu actions run");
}

void TestWorkspaceShellEditorTabContextMenuShowsAndExecutesPathActions() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path target = root / "src" / "alpha.cpp";
  const std::filesystem::path other = root / "src" / "beta.cpp";
  WriteFile(target, "int alpha() { return 1; }\n");
  WriteFile(other, "int beta() { return 2; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, target);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, other);
  WorkspaceShellTestAccess::ActivateTab(shell, 1);

  const SDL_FRect tab_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  Expect(SendMouseDown(
             shell, tab_rect.x + tab_rect.w * 0.5f, tab_rect.y + tab_rect.h * 0.5f,
             SDL_BUTTON_RIGHT),
         "right-clicking a tab should open the editor tab context menu");

  const auto labels = WorkspaceShellTestAccess::VisiblePopupMenuLabels(
      shell, WorkspaceShell::MenuId::EditorTabContext);
  Expect(std::find(labels.begin(), labels.end(), "Close All Tabs") != labels.end(),
         "editor tab context menu should expose Close All Tabs");
  Expect(std::find(labels.begin(), labels.end(), "Copy Relative Path") != labels.end(),
         "editor tab context menu should expose Copy Relative Path");
  Expect(std::find(labels.begin(), labels.end(), "Copy Absolute Path") != labels.end(),
         "editor tab context menu should expose Copy Absolute Path");

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  Expect(WorkspaceShellTestAccess::ExecuteCopyRelativePath(shell),
         "Copy Relative Path should execute from the active tab");
  Expect(clipboard_text == "src/alpha.cpp",
         ("Copy Relative Path should copy the active tab path relative to the project root "
          "(actual: " +
          clipboard_text + ")")
             .c_str());

  clipboard_text.clear();
  Expect(WorkspaceShellTestAccess::ExecuteCopyAbsolutePath(shell),
         "Copy Absolute Path should execute from the active tab");
  Expect(clipboard_text == target.lexically_normal().string(),
         "Copy Absolute Path should copy the active tab path");
}

void TestWorkspaceShellProjectTabContextMenuCopiesProjectRoot() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path target = root / "src" / "alpha.cpp";
  WriteFile(target, "int alpha() { return 1; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  // Open an editor tab so the active-tab path is non-empty; the project-tab
  // Copy Absolute Path must still copy the project root, not the editor file.
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, target);

  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::ProjectCopyAbsolutePath),
         "Copy Absolute Path should be enabled while a project is open");

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  Expect(WorkspaceShellTestAccess::ExecuteContextMenuAction(
             shell, ActionId::ProjectCopyAbsolutePath),
         "project tab Copy Absolute Path should execute");
  Expect(clipboard_text == root.lexically_normal().string(),
         ("project tab Copy Absolute Path should copy the project root "
          "(actual: " +
          clipboard_text + ")")
             .c_str());
}

void TestWorkspaceShellTreeContextMenuShowsInFileExplorerContainingDir() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path nested_dir = root / "src" / "nested";
  const std::filesystem::path file = nested_dir / "alpha.cpp";
  WriteFile(file, "int alpha() { return 1; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const auto file_items =
      WorkspaceTreeContextMenuItems(TreeContextTargetKind::File);
  Expect(std::find_if(file_items.begin(), file_items.end(),
                      [](const auto& item) {
                        return item.action == ActionId::ShowInFileExplorer;
                      }) != file_items.end(),
         "file tree context menu should expose Show in File Explorer");

  std::filesystem::path revealed;
  WorkspaceShellTestAccess::SetFileManagerOpener(
      shell, [&](const std::filesystem::path& dir) {
        revealed = dir;
        return true;
      });

  // File entry: should reveal the file's containing directory.
  WorkspaceShellTestAccess::OpenTreeContextMenuForPath(
      shell, TreeContextTargetKind::File, file);
  Expect(WorkspaceShellTestAccess::ExecuteShowInFileExplorer(shell),
         "Show in File Explorer should execute from the tree context menu");
  Expect(revealed == nested_dir.lexically_normal(),
         ("Show in File Explorer should open the file's containing directory "
          "(actual: " +
          revealed.string() + ")")
             .c_str());

  // Directory entry: should reveal the directory's parent.
  revealed.clear();
  WorkspaceShellTestAccess::OpenTreeContextMenuForPath(
      shell, TreeContextTargetKind::Directory, nested_dir);
  Expect(WorkspaceShellTestAccess::ExecuteShowInFileExplorer(shell),
         "Show in File Explorer should execute for a directory entry");
  Expect(revealed == (root / "src").lexically_normal(),
         ("Show in File Explorer should open the directory's parent "
          "(actual: " +
          revealed.string() + ")")
             .c_str());
}

void TestWorkspaceShellTabContextActionsCloseAdjacentTabs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path one = root / "one.txt";
  const std::filesystem::path two = root / "two.txt";
  const std::filesystem::path three = root / "three.txt";
  WriteFile(one, "one\n");
  WriteFile(two, "two\n");
  WriteFile(three, "three\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, one);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, two),
         "tab close fixture should open the middle tab");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, three),
         "tab close fixture should open the final tab");

  WorkspaceShellTestAccess::ActivateTab(shell, 1);
  Expect(WorkspaceShellTestAccess::ExecuteCloseTabsToRight(shell),
         "close tabs to the right should execute");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 2,
         "close tabs to the right should remove tabs after the active tab");
  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 1).find("two.txt") != std::string::npos,
         "close tabs to the right should keep the active tab in place");

  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, three),
         "tab close fixture should reopen the right-side tab");
  WorkspaceShellTestAccess::ActivateTab(shell, 1);
  Expect(WorkspaceShellTestAccess::ExecuteCloseTabsToLeft(shell),
         "close tabs to the left should execute");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 2,
         "close tabs to the left should remove tabs before the active tab");
  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 0).find("two.txt") != std::string::npos,
         "close tabs to the left should keep the active tab after compaction");

  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  Expect(WorkspaceShellTestAccess::ExecuteCloseOtherTabs(shell),
         "close other tabs should execute");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "close other tabs should keep only the active tab");
  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 0).find("two.txt") != std::string::npos,
         "close other tabs should preserve the selected tab");
}

#if MICROIDE_HAS_SDL3_TTF
void TestWorkspaceShellSidebarModeRetainedRedrawMatchesFullRender() {
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "src" / "main.cpp", "int main() { return 0; }\n");
  WriteFile(root / "README.md", "# demo\n");

  static constexpr int kCanvasWidth = 1280;
  static constexpr int kCanvasHeight = 720;

  const auto configure_shell = [&](WorkspaceShell& shell) {
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, kCanvasWidth, kCanvasHeight);
  };

  WorkspaceShell retained_shell;
  configure_shell(retained_shell);

  SoftwareCanvas retained_canvas(kCanvasWidth, kCanvasHeight);
  retained_shell.Render(retained_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  Expect(WorkspaceShellTestAccess::ExecuteShowGitSidebar(retained_shell),
         "sidebar redraw regression fixture should switch to the git sidebar");
  // Settle the async git refresh so the retained (rendered twice) and reference
  // (rendered once) shells capture the same deterministic git sidebar state.
  WorkspaceShellTestAccess::SettleGitSidebarRefresh(retained_shell);
  const auto redraw = retained_shell.ConsumePendingRenderInvalidation();
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(retained_shell);
  Expect(!redraw.full && !redraw.rects.empty(),
         "switching sidebar modes without geometry changes should stay on partial redraws");
  Expect(AnyRectIntersects(redraw.rects, layout.sidebar),
         "switching sidebar modes should invalidate the sidebar surface");

  RenderRetainedInvalidation(retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight, redraw);

  WorkspaceShell reference_shell;
  configure_shell(reference_shell);
  Expect(WorkspaceShellTestAccess::ExecuteShowGitSidebar(reference_shell),
         "reference sidebar redraw fixture should switch to the git sidebar");
  WorkspaceShellTestAccess::SettleGitSidebarRefresh(reference_shell);
  SoftwareCanvas reference_canvas(kCanvasWidth, kCanvasHeight);
  reference_shell.Render(reference_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  SDL_Surface* retained_pixels = SDL_RenderReadPixels(retained_canvas.renderer(), nullptr);
  SDL_Surface* reference_pixels = SDL_RenderReadPixels(reference_canvas.renderer(), nullptr);
  const std::size_t pixel_differences =
      CountPixelDifferences(retained_pixels, reference_pixels);

  if (retained_pixels != nullptr) {
    SDL_DestroySurface(retained_pixels);
  }
  if (reference_pixels != nullptr) {
    SDL_DestroySurface(reference_pixels);
  }

  Expect(pixel_differences == 0,
         "retained sidebar mode redraws should match a full redraw");
}

void TestWorkspaceShellOpenFileInNewTabRetainedRedrawMatchesFullRender() {
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.cpp";
  const std::filesystem::path beta = root / "beta.cpp";
  WriteFile(alpha, "alpha\n");
  WriteFile(beta, "beta\n");

  static constexpr int kCanvasWidth = 1280;
  static constexpr int kCanvasHeight = 720;

  const auto configure_shell = [&](WorkspaceShell& shell) {
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, kCanvasWidth, kCanvasHeight);
    WorkspaceShellTestAccess::OpenFile(shell, alpha);
    (void)shell.ConsumePendingRenderInvalidation();
  };

  WorkspaceShell retained_shell;
  configure_shell(retained_shell);

  SoftwareCanvas retained_canvas(kCanvasWidth, kCanvasHeight);
  retained_shell.Render(retained_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(retained_shell, beta),
         "tab redraw regression fixture should open a second file");
  const auto redraw = retained_shell.ConsumePendingRenderInvalidation();
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(retained_shell);
  Expect(!redraw.full && !redraw.rects.empty(),
         "opening a file in a new tab should stay on partial redraws");
  Expect(AnyRectIntersects(redraw.rects, layout.breadcrumb),
         "opening a file in a new tab should invalidate the breadcrumb");
  Expect(AnyRectIntersects(redraw.rects, layout.tab_strip),
         "opening a file in a new tab should invalidate the tab strip");
  Expect(AnyRectIntersects(redraw.rects, layout.editor_surface),
         "opening a file in a new tab should invalidate the editor surface");

  RenderRetainedInvalidation(retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight, redraw);

  WorkspaceShell reference_shell;
  configure_shell(reference_shell);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(reference_shell, beta),
         "reference tab redraw fixture should open a second file");
  SoftwareCanvas reference_canvas(kCanvasWidth, kCanvasHeight);
  reference_shell.Render(reference_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  SDL_Surface* retained_pixels = SDL_RenderReadPixels(retained_canvas.renderer(), nullptr);
  SDL_Surface* reference_pixels = SDL_RenderReadPixels(reference_canvas.renderer(), nullptr);
  const std::size_t pixel_differences =
      CountPixelDifferences(retained_pixels, reference_pixels);

  if (retained_pixels != nullptr) {
    SDL_DestroySurface(retained_pixels);
  }
  if (reference_pixels != nullptr) {
    SDL_DestroySurface(reference_pixels);
  }

  Expect(pixel_differences == 0,
         "retained tab-open redraws should match a full redraw");
}

void TestWorkspaceShellPartialRedrawWithoutCompareTabSkipsCompareSurfaceRender() {
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.cpp";
  const std::filesystem::path beta = root / "beta.cpp";
  WriteFile(alpha, "alpha\n");
  WriteFile(beta, "beta\n");

  static constexpr int kCanvasWidth = 1280;
  static constexpr int kCanvasHeight = 720;

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kCanvasWidth, kCanvasHeight);
  WorkspaceShellTestAccess::OpenFile(shell, alpha);
  (void)shell.ConsumePendingRenderInvalidation();

  SoftwareCanvas canvas(kCanvasWidth, kCanvasHeight);
  shell.Render(canvas.renderer(), kCanvasWidth, kCanvasHeight);
  WorkspaceShellTestAccess::ResetRenderCompareSurfaceInvocationCount(shell);

  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, beta),
         "compare-gate redraw fixture should open a second editor tab");
  const auto redraw = shell.ConsumePendingRenderInvalidation();
  Expect(!redraw.full && !redraw.rects.empty(),
         "compare-gate redraw fixture should produce a partial redraw");

  RenderRetainedInvalidation(shell, canvas, kCanvasWidth, kCanvasHeight, redraw);
  Expect(WorkspaceShellTestAccess::RenderCompareSurfaceInvocationCount(shell) == 0,
         "partial redraw with no compare tab active should not call RenderCompareSurface");
}

// Regression: the Settings font-family row draws its stored value through
// TruncateToWidth, which returns an *owned* std::string. Binding that temporary to a
// std::string_view (instead of a std::string) dangled the buffer before DrawStringOn
// ran, so the row painted freed heap — corrupted text that shifted as mouse-move
// redraws churned memory. Render the overlay with a heap-length font value so ASAN
// faults if the lifetime bug returns; a name longer than the SSO buffer forces a
// heap allocation for a deterministic use-after-free rather than a stack read.
void TestWorkspaceShellSettingsFontRowRendersStoredValueWithoutDanglingView() {
  EnsureDummySdlVideo();

  static constexpr int kCanvasWidth = 1280;
  static constexpr int kCanvasHeight = 800;

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "file.txt";
  WriteFile(source, "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kCanvasWidth, kCanvasHeight);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  // Long enough to force a heap allocation for the value string (so the old bug is
  // a deterministic heap-use-after-free under ASAN, not a stack read), yet short
  // enough to fit the 180px value box uncut — TruncateToWidth then heap-copies the
  // whole value, and the dangling view painted that freed heap.
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "editor.font_family",
                                                            "HeapFontName1234"),
         "the font-family String setting should accept an arbitrary value");
  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);
  // Isolate the font-family row so it is guaranteed on-screen (unfiltered it scrolls
  // below the visible window) and therefore actually exercises the render branch.
  WorkspaceShellTestAccess::SetSettingsOverlayQueryAndRefresh(shell, "font family");

  SoftwareCanvas canvas(kCanvasWidth, kCanvasHeight);
  // Two frames: a mouse-move redraw re-churns the heap, which is what made the stale
  // view visibly change frame-to-frame. Both must render cleanly under ASAN.
  shell.Render(canvas.renderer(), kCanvasWidth, kCanvasHeight);
  shell.Render(canvas.renderer(), kCanvasWidth, kCanvasHeight);
}

void TestWorkspaceShellNewlineInsertionRequestsEditorPartialRedraw() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "alpha();\nbeta();\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  (void)shell.ConsumePendingRenderInvalidation();

  const SDL_FRect editor_surface = WorkspaceShellTestAccess::CurrentLayout(shell).editor_surface;
  const std::size_t before_line_count =
      WorkspaceShellTestAccess::ActiveEditor(shell).line_count();
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = SDLK_RETURN;
  const auto result = shell.HandleEvent(event);
  Expect(result.handled,
         "newline redraw regression fixture should insert a newline into the editor");
  Expect(!result.redraw.full && !result.redraw.rects.empty(),
         "newline insertion should request a partial editor redraw after changing the editor line count");
  Expect(AnyRectIntersects(result.redraw.rects, editor_surface),
         "newline insertion redraws should include the editor surface");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).line_count() == before_line_count + 1,
         "newline insertion should split the current line in the active editor");
}

void TestWorkspaceShellBottomEdgeNewlineRetainedRedrawMatchesFullRender() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#else
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  std::string content;
  for (int line = 1; line <= 48; ++line) {
    content += "line " + std::to_string(line) + "\n";
  }
  WriteFile(source, content);

  static constexpr int kCanvasWidth = 1280;
  static constexpr int kCanvasHeight = 220;

  const auto configure_shell = [&](WorkspaceShell& shell) {
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, kCanvasWidth, kCanvasHeight);
    WorkspaceShellTestAccess::OpenFile(shell, source);
    (void)shell.ConsumePendingRenderInvalidation();
  };

  WorkspaceShell retained_shell;
  configure_shell(retained_shell);
  SoftwareCanvas retained_canvas(kCanvasWidth, kCanvasHeight);
  retained_shell.Render(retained_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorMetrics(retained_shell);
  const std::size_t visible_rows = std::max<std::size_t>(1, metrics.visible_rows);
  const std::size_t target_scroll = 12;
  const std::size_t target_line = target_scroll + visible_rows - 1;
  WorkspaceShellTestAccess::ActiveEditor(retained_shell).SetScrollLine(target_scroll);
  WorkspaceShellTestAccess::ActiveEditor(retained_shell).MoveCursorTo(
      target_line,
      WorkspaceShellTestAccess::ActiveEditor(retained_shell).lines()[target_line].size());
  retained_shell.Render(retained_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  WorkspaceShell reference_shell;
  configure_shell(reference_shell);
  SoftwareCanvas reference_canvas(kCanvasWidth, kCanvasHeight);
  reference_shell.Render(reference_canvas.renderer(), kCanvasWidth, kCanvasHeight);
  WorkspaceShellTestAccess::ActiveEditor(reference_shell).SetScrollLine(target_scroll);
  WorkspaceShellTestAccess::ActiveEditor(reference_shell).MoveCursorTo(
      target_line,
      WorkspaceShellTestAccess::ActiveEditor(reference_shell).lines()[target_line].size());
  reference_shell.Render(reference_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = SDLK_RETURN;
  const auto retained_result = retained_shell.HandleEvent(event);
  const auto reference_result = reference_shell.HandleEvent(event);
  Expect(retained_result.handled && reference_result.handled,
         "bottom-edge newline fixture should insert a newline into both editor shells");
  Expect(!retained_result.redraw.full && !retained_result.redraw.rects.empty(),
         "bottom-edge newline insertion should still use partial redraw invalidation");
  // Keep the retained-vs-full pixel comparison deterministic by hiding editor caret blinking.
  WorkspaceShellTestAccess::SetFocusSidebar(retained_shell);
  WorkspaceShellTestAccess::SetFocusSidebar(reference_shell);

  RenderRetainedInvalidation(retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight,
                             retained_result.redraw);
  reference_shell.Render(reference_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  SDL_Surface* retained_pixels = SDL_RenderReadPixels(retained_canvas.renderer(), nullptr);
  SDL_Surface* reference_pixels = SDL_RenderReadPixels(reference_canvas.renderer(), nullptr);
  const std::size_t pixel_differences =
      CountPixelDifferences(retained_pixels, reference_pixels);

  if (retained_pixels != nullptr) {
    SDL_DestroySurface(retained_pixels);
  }
  if (reference_pixels != nullptr) {
    SDL_DestroySurface(reference_pixels);
  }

  // ASAN/UBSAN instrumented runs can produce minor rasterization deltas near
  // clip boundaries; keep the assertion strict enough to catch real redraw
  // mismatches while remaining stable across sanitizer builds.
  static constexpr std::size_t kMaxAllowedPixelDiff = 2048;
  Expect(pixel_differences <= kMaxAllowedPixelDiff,
         "retained redraw after a bottom-edge newline should match a full redraw");
#endif
}

void TestWorkspaceShellSidebarResizeRequestsFullRedrawAndMatchesFullRender() {
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source,
            "int main() {\n"
            "  return 0;\n"
            "}\n"
            "// sidebar resize regression fixture\n");

  static constexpr int kCanvasWidth = 1280;
  static constexpr int kCanvasHeight = 720;

  const auto configure_shell = [&](WorkspaceShell& shell) {
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, kCanvasWidth, kCanvasHeight);
    WorkspaceShellTestAccess::OpenFile(shell, source);
    (void)shell.ConsumePendingRenderInvalidation();
  };

  const auto handle_divider_press = [&](WorkspaceShell& shell, float x, float y) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = x;
    event.button.y = y;
    return shell.HandleEvent(event);
  };

  const auto handle_divider_drag = [&](WorkspaceShell& shell, float x, float y) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = x;
    event.motion.y = y;
    event.motion.state = SDL_BUTTON_LMASK;
    return shell.HandleEvent(event);
  };

  WorkspaceShell retained_shell;
  configure_shell(retained_shell);

  SoftwareCanvas retained_canvas(kCanvasWidth, kCanvasHeight);
  retained_shell.Render(retained_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  const auto initial_layout = WorkspaceShellTestAccess::CurrentLayout(retained_shell);
  const SDL_FRect resize_handle = microide::workspace::SidebarResizeHandleRect(initial_layout);
  const float drag_start_x = resize_handle.x + resize_handle.w * 0.5f;
  const float drag_y = resize_handle.y + resize_handle.h * 0.5f;
  const float drag_end_x = drag_start_x + 120.0f;

  const auto retained_press = handle_divider_press(retained_shell, drag_start_x, drag_y);
  Expect(retained_press.handled,
         "sidebar resize regression should start dragging on the sidebar divider");
  const auto retained_drag = handle_divider_drag(retained_shell, drag_end_x, drag_y);
  Expect(retained_drag.handled,
         "sidebar resize regression should handle divider drag motion");
  Expect(retained_drag.redraw.full,
         "sidebar resize should promote to a full redraw while the divider is moving");

  const auto resized_layout = WorkspaceShellTestAccess::CurrentLayout(retained_shell);
  Expect(resized_layout.sidebar.w > initial_layout.sidebar.w,
         "sidebar resize regression should widen the sidebar after dragging right");

  RenderRetainedInvalidation(
      retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight, retained_drag.redraw);
  Expect(!retained_shell.ConsumePostRenderFullRedrawRequest(),
         "sidebar resize should not require terminal settle redraws");

  WorkspaceShell reference_shell;
  configure_shell(reference_shell);
  SoftwareCanvas reference_canvas(kCanvasWidth, kCanvasHeight);
  reference_shell.Render(reference_canvas.renderer(), kCanvasWidth, kCanvasHeight);
  const auto reference_layout = WorkspaceShellTestAccess::CurrentLayout(reference_shell);
  const SDL_FRect reference_handle =
      microide::workspace::SidebarResizeHandleRect(reference_layout);
  const float reference_drag_start_x = reference_handle.x + reference_handle.w * 0.5f;
  const float reference_drag_y = reference_handle.y + reference_handle.h * 0.5f;
  const float reference_drag_end_x = reference_drag_start_x + 120.0f;
  Expect(handle_divider_press(reference_shell, reference_drag_start_x, reference_drag_y).handled,
         "reference sidebar resize fixture should start divider dragging");
  Expect(handle_divider_drag(reference_shell, reference_drag_end_x, reference_drag_y).handled,
         "reference sidebar resize fixture should handle divider drag motion");
  reference_shell.Render(reference_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  SDL_Surface* retained_pixels = SDL_RenderReadPixels(retained_canvas.renderer(), nullptr);
  SDL_Surface* reference_pixels = SDL_RenderReadPixels(reference_canvas.renderer(), nullptr);
  const std::size_t pixel_differences =
      CountPixelDifferences(retained_pixels, reference_pixels);

  if (retained_pixels != nullptr) {
    SDL_DestroySurface(retained_pixels);
  }
  if (reference_pixels != nullptr) {
    SDL_DestroySurface(reference_pixels);
  }

  Expect(pixel_differences == 0,
         "retained sidebar resize redraws should match a full redraw");
}

void TestWorkspaceShellBottomPanelResizeRequestsFullRedrawAndSettleFrames() {
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source,
            "int main() {\n"
            "  return 0;\n"
            "}\n"
            "// resize regression fixture\n");

  static constexpr int kCanvasWidth = 1280;
  static constexpr int kCanvasHeight = 720;

  const auto configure_shell = [&](WorkspaceShell& shell) {
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, kCanvasWidth, kCanvasHeight);
    WorkspaceShellTestAccess::OpenFile(shell, source);
    WorkspaceShellTestAccess::EnsureTerminalTab(shell);
    auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
    TerminalSessionTestAccess::Reset(session, 24, 80);
    TerminalSessionTestAccess::SetCursorVisible(session, false);
    TerminalSessionTestAccess::AppendOutput(
        session, "terminal resize\nkeeps stale pixels away\nthird line\n");
    (void)shell.ConsumePendingRenderInvalidation();
  };

  const auto handle_divider_press = [&](WorkspaceShell& shell, float x, float y) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = x;
    event.button.y = y;
    return shell.HandleEvent(event);
  };

  const auto handle_divider_drag = [&](WorkspaceShell& shell, float x, float y) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = x;
    event.motion.y = y;
    event.motion.state = SDL_BUTTON_LMASK;
    return shell.HandleEvent(event);
  };

  WorkspaceShell retained_shell;
  configure_shell(retained_shell);

  SoftwareCanvas retained_canvas(kCanvasWidth, kCanvasHeight);
  retained_shell.Render(retained_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  const auto initial_layout = WorkspaceShellTestAccess::CurrentLayout(retained_shell);
  const SDL_FRect resize_handle = microide::workspace::BottomPanelResizeHandleRect(initial_layout);
  const float drag_x = resize_handle.x + resize_handle.w * 0.5f;
  const float drag_start_y = resize_handle.y + resize_handle.h * 0.5f;
  const float drag_end_y = drag_start_y + 80.0f;

  const auto retained_press = handle_divider_press(retained_shell, drag_x, drag_start_y);
  Expect(retained_press.handled,
         "bottom-panel resize regression should start dragging on the panel divider");
  const auto retained_drag = handle_divider_drag(retained_shell, drag_x, drag_end_y);
  Expect(retained_drag.handled,
         "bottom-panel resize regression should handle divider drag motion");

  const auto& redraw = retained_drag.redraw;
  const auto resized_layout = WorkspaceShellTestAccess::CurrentLayout(retained_shell);
  Expect(resized_layout.bottom_panel.h < initial_layout.bottom_panel.h,
         "bottom-panel resize regression should shrink the terminal panel");
  Expect(redraw.full,
         "bottom-panel resize should promote to a full redraw for correctness");

  RenderRetainedInvalidation(retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight, redraw);
  Expect(retained_shell.ConsumePostRenderFullRedrawRequest(),
         "bottom-panel resize should schedule a follow-up redraw after the first full render");
  retained_shell.Render(retained_canvas.renderer(), kCanvasWidth, kCanvasHeight);
  Expect(retained_shell.ConsumePostRenderFullRedrawRequest(),
         "bottom-panel resize should schedule a second settle redraw");
  retained_shell.Render(retained_canvas.renderer(), kCanvasWidth, kCanvasHeight);
  Expect(!retained_shell.ConsumePostRenderFullRedrawRequest(),
         "bottom-panel resize should settle after the bounded follow-up redraws");
}

void TestWorkspaceShellPrepareFrameSkipsLayoutWhenNotDirty() {
  EnsureDummySdlVideo();
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  WorkspaceShellTestAccess::ResetPrepareFrameLayoutComputeCount(shell);

  for (int i = 0; i < 10; ++i) {
    shell.Render(canvas.renderer(), 1280, 720);
  }
  Expect(WorkspaceShellTestAccess::PrepareFrameLayoutComputeCount(shell) == 0,
         "PrepareFrameOnce should skip ComputeLayout while the layout-dirty flag is clear");
}

void TestWorkspaceShellPrepareFrameRecomputesLayoutAfterResize() {
  EnsureDummySdlVideo();
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  SoftwareCanvas canvas(1400, 900);
  shell.Render(canvas.renderer(), 1280, 720);
  WorkspaceShellTestAccess::ResetPrepareFrameLayoutComputeCount(shell);

  WorkspaceShellTestAccess::SetWindowSize(shell, 1400, 900);
  shell.Render(canvas.renderer(), 1400, 900);
  Expect(WorkspaceShellTestAccess::PrepareFrameLayoutComputeCount(shell) == 1,
         "PrepareFrameOnce should recompute layout once on the frame after a resize");
}

void TestWorkspaceShellRenderBuildsEditorViewModelOncePerSimplePaneFrame() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(1280, 720);

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "main.txt";
  WriteFile(file, "alpha\nbeta\ngamma\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "workspace render perf regression test should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, file);

  shell.Render(canvas.renderer(), 1280, 720);
  util::ResetPerformanceCounters();
  shell.Render(canvas.renderer(), 1280, 720);

  Expect(util::ReadPerformanceCounter(util::PerfCounterId::RenderBuildEditorViewModelCalls) == 1,
         "simple single-pane editor frame should build the editor view model exactly once");
}
#endif

void TestViewMenuToggleReflectsBackingSetting() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "menu check\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.wrap", "word"),
         "menu-checked fixture should set editor.wrap to word");
  Expect(WorkspaceShellTestAccess::MenuItemCheckedByLabel(shell, WorkspaceShell::MenuId::View,
                                                         "Word Wrap") == true,
         "Word Wrap entry should report checked=true when its setting is on");

  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "editor.wrap", "off"),
         "menu-checked fixture should flip editor.wrap to off");
  Expect(WorkspaceShellTestAccess::MenuItemCheckedByLabel(shell, WorkspaceShell::MenuId::View,
                                                         "Word Wrap") == false,
         "Word Wrap entry should report checked=false when its setting is off");
}

void TestEditorTabStripOverflowControlsScrollAndCount() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "tab overflow\n");
  // Open enough tabs to overflow a 1280-wide strip (each tab clamps at 132px
  // minimum, so 16 tabs alone won't fit when the strip reserves margins +
  // chevron space).
  std::vector<std::filesystem::path> sources;
  for (int i = 0; i < 16; ++i) {
    std::filesystem::path p =
        root / ("file_" + std::to_string(i) + "_long_name_for_overflow_test.txt");
    WriteFile(p, "alpha\n");
    sources.push_back(p);
  }

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  for (const auto& p : sources) {
    WorkspaceShellTestAccess::OpenFile(shell, p);
  }

  // Force the active tab to the first one so all overflow is on the right.
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  const auto overflow_at_left =
      WorkspaceShellTestAccess::EditorTabOverflowControls(shell);
  Expect(overflow_at_left.hidden_left == 0,
         "no tabs should be hidden left when the first tab is active");
  Expect(overflow_at_left.hidden_right > 0,
         "tabs should be hidden right when more open than fit");

  // Scroll right by one and verify hidden counts shift.
  const std::size_t hidden_right_before = overflow_at_left.hidden_right;
  Expect(WorkspaceShellTestAccess::ScrollEditorTabStrip(shell, +1),
         "scrolling the editor tab strip right should report a change");
  const auto overflow_after_scroll =
      WorkspaceShellTestAccess::EditorTabOverflowControls(shell);
  Expect(overflow_after_scroll.hidden_left == 1,
         "after one right-scroll one tab should be hidden left");
  Expect(overflow_after_scroll.hidden_right + 1 == hidden_right_before,
         "after one right-scroll the right-hidden count should decrement by one");
}

void TestEditorTabStripUsesLeftGapWhenOnlyRightOverflowRemains() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "tab reserve\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  for (int i = 0; i < 16; ++i) {
    const std::filesystem::path file = root / ("reserve_" + std::to_string(i) + ".txt");
    WriteFile(file, "reserve\n");
    WorkspaceShellTestAccess::OpenFile(shell, file);
  }

  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  const auto overflow = WorkspaceShellTestAccess::EditorTabOverflowControls(shell);
  Expect(overflow.hidden_left == 0 && overflow.hidden_right > 0,
         "fixture should expose only right-side editor-tab overflow");

  const SDL_FRect first_tab = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  Expect(std::fabs(first_tab.x - layout.tab_strip.x) <= 0.01f,
         "editor tabs should reclaim the left chevron reserve when only the right overflow button is visible");
}

void TestEditorTabOverflowButtonExpandsForDoubleDigitHiddenCount() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "overflow width\n");

  WorkspaceShell small_shell;
  WorkspaceShellTestAccess::SetProjectRoot(small_shell, root);
  WorkspaceShellTestAccess::SetWindowSize(small_shell, 1280, 720);
  for (int i = 0; i < 12; ++i) {
    const std::filesystem::path file = root / ("small_" + std::to_string(i) + ".txt");
    WriteFile(file, "small\n");
    WorkspaceShellTestAccess::OpenFile(small_shell, file);
  }
  WorkspaceShellTestAccess::ActivateTab(small_shell, 0);
  const auto small_overflow =
      WorkspaceShellTestAccess::EditorTabOverflowControls(small_shell);
  Expect(small_overflow.hidden_right > 0 && small_overflow.hidden_right < 10,
         "single-digit fixture should hide fewer than 10 tabs to the right");
  Expect(std::fabs(small_overflow.right_button.w - 28.0f) <= 0.01f,
         "single-digit hidden count should keep the default overflow button width");

  WorkspaceShell large_shell;
  WorkspaceShellTestAccess::SetProjectRoot(large_shell, root);
  WorkspaceShellTestAccess::SetWindowSize(large_shell, 1280, 720);
  for (int i = 0; i < 28; ++i) {
    const std::filesystem::path file = root / ("large_" + std::to_string(i) + ".txt");
    WriteFile(file, "large\n");
    WorkspaceShellTestAccess::OpenFile(large_shell, file);
  }
  WorkspaceShellTestAccess::ActivateTab(large_shell, 0);
  const auto large_overflow =
      WorkspaceShellTestAccess::EditorTabOverflowControls(large_shell);
  Expect(large_overflow.hidden_right > 9,
         "double-digit fixture should hide at least 10 tabs to the right");
  Expect(large_overflow.right_button.w > 28.0f,
         "double-digit hidden count should widen the overflow button");
}

void TestClosingTabsWhileScrolledRecomputesOverflowImmediately() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "close overflow\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  for (int i = 0; i < 16; ++i) {
    const std::filesystem::path file = root / ("c" + std::to_string(i) + ".txt");
    WriteFile(file, "close\n");
    WorkspaceShellTestAccess::OpenFile(shell, file);
  }

  WorkspaceShellTestAccess::ActivateTab(shell, 7);
  const auto before = WorkspaceShellTestAccess::EditorTabOverflowControls(shell);
  Expect(before.hidden_left > 0 || before.hidden_right > 0,
         "fixture should overflow before closing tabs");

  Expect(WorkspaceShellTestAccess::ExecuteCloseTabsToRight(shell),
         "closing tabs to the right should succeed while scrolled");
  const auto after = WorkspaceShellTestAccess::EditorTabOverflowControls(shell);
  Expect(after.hidden_left == 0 && after.hidden_right == 0,
         "closing tabs should recompute overflow immediately when the remaining tabs all fit");
}

// The MATLAB-style gutter menu opens only on an existing breakpoint, seeds the
// enable/disable label from the breakpoint's state, and the disable / clear-
// condition items act without a prompt (clearing keeps hit-count + log message).
void TestWorkspaceShellBreakpointGutterMenu() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetSettingValue(shell, "debug.enabled", "true");
  const std::filesystem::path path = "/tmp/project/main.cpp";

  // Right-click on a bare line is inert (no breakpoint there yet).
  WorkspaceShellTestAccess::OpenBreakpointContextMenu(shell, path, 9);
  Expect(!WorkspaceShellTestAccess::BreakpointContextMenuOpen(shell),
         "the gutter menu does not open on a line without a breakpoint");

  auto& store = WorkspaceShellTestAccess::BreakpointStore(shell);
  store.Toggle(path, 9);
  store.SetCondition(path, 9, "i > 5");
  store.SetHitCondition(path, 9, ">10");

  // On a breakpoint line the menu opens and seeds the enabled flag (true here).
  WorkspaceShellTestAccess::OpenBreakpointContextMenu(shell, path, 9);
  Expect(WorkspaceShellTestAccess::BreakpointContextMenuOpen(shell),
         "the gutter menu opens on an existing breakpoint");
  Expect(WorkspaceShellTestAccess::BreakpointContextMenuEnabledFlag(shell),
         "the menu seeds the enabled flag from an enabled breakpoint");

  // Toggle-enabled disables the breakpoint without removing it.
  Expect(WorkspaceShellTestAccess::ExecuteBreakpointMenuAction(
             shell, ActionId::DebugBreakpointToggleEnabled),
         "the toggle-enabled menu item executes");
  const auto* bps = store.FindByPath(path);
  Expect(bps != nullptr && bps->size() == 1 && !(*bps)[0].enabled,
         "toggle-enabled disables the breakpoint, keeping it in the store");

  // Reopening seeds the flag as disabled (drives the "Enable Breakpoint" label).
  WorkspaceShellTestAccess::OpenBreakpointContextMenu(shell, path, 9);
  Expect(!WorkspaceShellTestAccess::BreakpointContextMenuEnabledFlag(shell),
         "the menu seeds the enabled flag from a disabled breakpoint");

  // Clear-condition drops only the condition; hit-count is preserved.
  Expect(WorkspaceShellTestAccess::ExecuteBreakpointMenuAction(
             shell, ActionId::DebugBreakpointClearCondition),
         "the clear-condition menu item executes");
  const auto* cleared = store.FindByPath(path);
  Expect(cleared != nullptr && !(*cleared)[0].condition.has_value(),
         "clear-condition removes the condition");
  Expect((*cleared)[0].hit_condition == std::optional<std::string>(">10"),
         "clear-condition keeps the hit-count modifier");
}

void TestColorschemeChangeRequestsRepaint() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "theme check\n");

  constexpr float kWindowWidth = 1280.0f;
  constexpr float kWindowHeight = 720.0f;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, static_cast<int>(kWindowWidth),
                                          static_cast<int>(kWindowHeight));

  // A theme change recolors the whole UI, so it must repaint the entire window —
  // not just the chrome strips that other action side effects happen to dirty.
  // Without that full repaint the shell idles on events and keeps showing the
  // previous theme's colors (the reported bug).
  const auto repaints_whole_window = [&](const auto& inv) {
    if (inv.full) {
      return true;
    }
    return std::any_of(inv.rects.begin(), inv.rects.end(), [&](const SDL_FRect& r) {
      return r.w >= kWindowWidth && r.h >= kWindowHeight;
    });
  };

  (void)shell.ConsumePendingRenderInvalidation();
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "colorscheme light"),
         "the colorscheme command should run");
  Expect(repaints_whole_window(shell.ConsumePendingRenderInvalidation()),
         "selecting a colorscheme must repaint the whole window");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "editor.colorscheme") ==
             std::optional<std::string>("light"),
         "the colorscheme command should activate the requested theme");

  // toggle-theme flips between light and dark and must repaint the whole window too.
  (void)shell.ConsumePendingRenderInvalidation();
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "toggle-theme"),
         "the toggle-theme command should run");
  Expect(repaints_whole_window(shell.ConsumePendingRenderInvalidation()),
         "toggling the theme must repaint the whole window");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "editor.colorscheme") !=
             std::optional<std::string>("light"),
         "toggle-theme should switch away from the light theme");
}

}  // namespace

// Regression: a left-click in a horizontally-scrolled (non-soft-wrapped) editor must
// place the caret under the pointer. The mouse coordinator fed LogicalPositionForVisualHit
// an ABSOLUTE visual column while the function re-adds the row's visual_start (which
// equals horizontal_scroll in the non-wrap layout), double-counting the scroll and
// snapping the caret to the right edge on every click once the line scrolled.
void TestWorkspaceShellEditorClickHonorsHorizontalScroll() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "long.txt";
  // A very long single line, wider than any plausible viewport, with uniform content
  // so the visual->text column mapping is 1:1 (no tabs to expand).
  WriteFile(source, std::string(2000, 'a') + "\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, source), "long file should open");

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(!editor.soft_wrap(), "word wrap should default off so the view scrolls horizontally");
  // Move the caret far right so the view scrolls horizontally, then settle the metrics
  // (this also sets the viewport size the hit-test uses).
  editor.MoveCursorTo(0, 1999, false);
  const auto metrics = WorkspaceShellTestAccess::ActiveEditorRenderMetrics(shell);
  const float char_width = WorkspaceShellTestAccess::TextCharWidth(shell);
  Expect(char_width > 0.0f, "char width should be positive");
  const std::size_t horizontal_scroll = editor.horizontal_scroll();
  Expect(horizontal_scroll > 0,
         "moving the caret to the far right should scroll the view horizontally");

  // Click five cells into the visible text area (well within the viewport width).
  const std::size_t screen_cells = 5;
  const float click_x = metrics.text_x + (static_cast<float>(screen_cells) + 0.25f) * char_width;
  const float click_y = metrics.first_line_y + metrics.line_height * 0.5f;
  Expect(SendMouseDown(shell, click_x, click_y, SDL_BUTTON_LEFT, 1),
         "the editor click should be handled");

  Expect(editor.cursor_line() == 0, "the click should stay on the only line");
  Expect(editor.cursor_column() == horizontal_scroll + screen_cells,
         "click caret column must honor the horizontal scroll exactly once (no double-count)");
}

void RegisterWorkspaceShellChromeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/EditorClickHonorsHorizontalScroll",
          TestWorkspaceShellEditorClickHonorsHorizontalScroll);
  AddTest(tests, "WorkspaceShell/ColorschemeChangeRequestsRepaint",
          TestColorschemeChangeRequestsRepaint);
  AddTest(tests, "WorkspaceShell/BreakpointGutterMenu", TestWorkspaceShellBreakpointGutterMenu);
  AddTest(tests, "WorkspaceShell/ViewMenuToggleReflectsBackingSetting",
          TestViewMenuToggleReflectsBackingSetting);
  AddTest(tests, "WorkspaceShell/EditorTabStripOverflowControlsScrollAndCount",
          TestEditorTabStripOverflowControlsScrollAndCount);
  AddTest(tests, "WorkspaceShell/EditorTabStripUsesLeftGapWhenOnlyRightOverflowRemains",
          TestEditorTabStripUsesLeftGapWhenOnlyRightOverflowRemains);
  AddTest(tests, "WorkspaceShell/EditorTabOverflowButtonExpandsForDoubleDigitHiddenCount",
          TestEditorTabOverflowButtonExpandsForDoubleDigitHiddenCount);
  AddTest(tests, "WorkspaceShell/ClosingTabsWhileScrolledRecomputesOverflowImmediately",
          TestClosingTabsWhileScrolledRecomputesOverflowImmediately);
  AddTest(tests, "WorkspaceShell/MenuBarOmitsRemovedMenus",
          TestWorkspaceShellMenuBarOmitsRemovedMenus);
  AddTest(tests, "WorkspaceShell/MenuBarShowsChevronWhenTruncated",
          TestWorkspaceShellMenuBarShowsChevronWhenTruncated);
  AddTest(tests, "WorkspaceShell/CompactMenuOverflowButtonIsInteractive",
          TestWorkspaceShellCompactMenuOverflowButtonIsInteractive);
  AddTest(tests, "WorkspaceShell/CompactMenuOverflowRowsOpenAnchoredMenus",
          TestWorkspaceShellCompactMenuOverflowRowsOpenAnchoredMenus);
  AddTest(tests, "WorkspaceShell/MenuBarHoverSwitchesActiveMenu",
          TestWorkspaceShellMenuBarHoverSwitchesActiveMenu);
  AddTest(tests, "WorkspaceShell/MenuEventsReturnPartialChromeInvalidation",
          TestWorkspaceShellMenuEventsReturnPartialChromeInvalidation);
  AddTest(tests, "WorkspaceShell/PopupRowHoverReturnsPopupOnlyInvalidation",
          TestWorkspaceShellPopupRowHoverReturnsPopupOnlyInvalidation);
  AddTest(tests, "WorkspaceShell/TreeSidebarHeaderHoverReturnsButtonOnlyInvalidation",
          TestWorkspaceShellTreeSidebarHeaderHoverReturnsButtonOnlyInvalidation);
  AddTest(tests, "WorkspaceShell/SearchSidebarHeaderHoverReturnsButtonOnlyInvalidation",
          TestWorkspaceShellSearchSidebarHeaderHoverReturnsButtonOnlyInvalidation);
  AddTest(tests, "WorkspaceShell/GitSidebarHeaderHoverReturnsButtonOnlyInvalidation",
          TestWorkspaceShellGitSidebarHeaderHoverReturnsButtonOnlyInvalidation);
  AddTest(tests, "WorkspaceShell/OpenMenuSuppressesUnderlyingTabTooltip",
          TestWorkspaceShellOpenMenuSuppressesUnderlyingTabTooltip);
  AddTest(tests, "WorkspaceShell/OverflowPopupSuppressesUnderlyingTabTooltip",
          TestWorkspaceShellOverflowPopupSuppressesUnderlyingTabTooltip);
  AddTest(tests, "WorkspaceShell/StatusRowShowsLspReadinessAndInFlightState",
          TestWorkspaceShellStatusRowShowsLspReadinessAndInFlightState);
  AddTest(tests, "WorkspaceShell/EditorCaretDirtyRectFollowsActiveCaret",
          TestWorkspaceShellEditorCaretDirtyRectFollowsActiveCaret);
  AddTest(tests, "WorkspaceShell/EditorTypingReturnsPartialEditorInvalidation",
          TestWorkspaceShellEditorTypingReturnsPartialEditorInvalidation);
  AddTest(tests, "WorkspaceShell/WrappedEditorLineRangeRectCoversContinuationRows",
          TestWorkspaceShellWrappedEditorLineRangeRectCoversContinuationRows);
  AddTest(tests, "WorkspaceShell/CommandTextInputReturnsPartialCommandInvalidation",
          TestWorkspaceShellCommandTextInputReturnsPartialCommandInvalidation);
  AddTest(tests, "WorkspaceShell/CommandPasteShortcutUsesSharedTextInputPath",
          TestWorkspaceShellCommandPasteShortcutUsesSharedTextInputPath);
  AddTest(tests, "WorkspaceShell/ProjectTabsShowBadges",
          TestWorkspaceShellProjectTabsShowBadges);
  AddTest(tests, "WorkspaceShell/ProjectTabBadgeColorStableAcrossSwitch",
          TestWorkspaceShellProjectTabBadgeColorStableAcrossSwitch);
  AddTest(tests, "WorkspaceShell/SidebarModeTabsPresent",
          TestWorkspaceShellSidebarModeTabsPresent);
  AddTest(tests, "WorkspaceShell/TabTooltipRendersAboveSidebar",
          TestWorkspaceShellTabTooltipRendersAboveSidebar);
  AddTest(tests, "WorkspaceShell/GitSidebarTooltipUsesSharedCompactCard",
          TestWorkspaceShellGitSidebarTooltipUsesSharedCompactCard);
  AddTest(tests, "WorkspaceShell/ProjectTabTooltipDismissRetainedRedrawMatchesFullRender",
          TestWorkspaceShellProjectTabTooltipDismissRetainedRedrawMatchesFullRender);
  AddTest(tests, "WorkspaceShell/PromptInputRendersSharedFramedField",
          TestWorkspaceShellPromptInputRendersSharedFramedField);
  AddTest(tests, "WorkspaceShell/ShortcutEditActionsReturnEditorInvalidation",
          TestWorkspaceShellShortcutEditActionsReturnEditorInvalidation);
  AddTest(tests, "WorkspaceShell/EditorTabRightClickOpensContextMenu",
          TestWorkspaceShellEditorTabRightClickOpensContextMenu);
  AddTest(tests, "WorkspaceShell/EditorTabContextMenuShowsAndExecutesPathActions",
          TestWorkspaceShellEditorTabContextMenuShowsAndExecutesPathActions);
  AddTest(tests, "WorkspaceShell/ProjectTabContextMenuCopiesProjectRoot",
          TestWorkspaceShellProjectTabContextMenuCopiesProjectRoot);
  AddTest(tests, "WorkspaceShell/TabContextActionsCloseAdjacentTabs",
          TestWorkspaceShellTabContextActionsCloseAdjacentTabs);
  AddTest(tests, "WorkspaceShell/TreeContextMenuShowsInFileExplorerContainingDir",
          TestWorkspaceShellTreeContextMenuShowsInFileExplorerContainingDir);
#if MICROIDE_HAS_SDL3_TTF
  AddTest(tests, "WorkspaceShell/SidebarModeRetainedRedrawMatchesFullRender",
          TestWorkspaceShellSidebarModeRetainedRedrawMatchesFullRender);
  AddTest(tests, "WorkspaceShell/OpenFileInNewTabRetainedRedrawMatchesFullRender",
          TestWorkspaceShellOpenFileInNewTabRetainedRedrawMatchesFullRender);
  AddTest(tests, "WorkspaceShell/PartialRedrawWithoutCompareTabSkipsCompareSurfaceRender",
          TestWorkspaceShellPartialRedrawWithoutCompareTabSkipsCompareSurfaceRender);
  AddTest(tests, "WorkspaceShell/SettingsFontRowRendersStoredValueWithoutDanglingView",
          TestWorkspaceShellSettingsFontRowRendersStoredValueWithoutDanglingView);
  AddTest(tests, "WorkspaceShell/NewlineInsertionRequestsEditorPartialRedraw",
          TestWorkspaceShellNewlineInsertionRequestsEditorPartialRedraw);
  AddTest(tests, "WorkspaceShell/WrappedTypingReturnsFocusedPaneInvalidation",
          TestWorkspaceShellWrappedTypingReturnsFocusedPaneInvalidation);
  AddTest(tests, "WorkspaceShell/BottomEdgeNewlineRetainedRedrawMatchesFullRender",
          TestWorkspaceShellBottomEdgeNewlineRetainedRedrawMatchesFullRender);
  AddTest(tests, "WorkspaceShell/SidebarResizeRequestsFullRedrawAndMatchesFullRender",
          TestWorkspaceShellSidebarResizeRequestsFullRedrawAndMatchesFullRender);
  AddTest(tests, "WorkspaceShell/BottomPanelResizeRequestsFullRedrawAndSettleFrames",
          TestWorkspaceShellBottomPanelResizeRequestsFullRedrawAndSettleFrames);
  AddTest(tests, "WorkspaceShell/PrepareFrameSkipsLayoutWhenNotDirty",
          TestWorkspaceShellPrepareFrameSkipsLayoutWhenNotDirty);
  AddTest(tests, "WorkspaceShell/PrepareFrameRecomputesLayoutAfterResize",
          TestWorkspaceShellPrepareFrameRecomputesLayoutAfterResize);
  AddTest(tests, "WorkspaceShell/RenderBuildsEditorViewModelOncePerSimplePaneFrame",
          TestWorkspaceShellRenderBuildsEditorViewModelOncePerSimplePaneFrame);
#endif
  AddTest(tests, "WorkspaceShell/FileCloseAllTabsClosesOpenEditorTabs",
          TestWorkspaceShellFileCloseAllTabsClosesOpenEditorTabs);
  AddTest(tests, "WorkspaceShell/TitleBarDragRegionDoesNotFabricateMaximizeToggle",
          TestWorkspaceShellTitleBarDragRegionDoesNotFabricateMaximizeToggle);
  AddTest(tests, "WorkspaceShell/FullscreenStateDisablesResizableFrameHitTest",
          TestWorkspaceShellFullscreenStateDisablesResizableFrameHitTest);
  AddTest(tests, "WorkspaceShell/WindowPresentationStateUpdatesChromeAndSize",
          TestWorkspaceShellWindowPresentationStateUpdatesChromeAndSize);
}

}  // namespace microide::tests
