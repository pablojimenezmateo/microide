#include "TestSupport.h"
#include "support/ProjectSearchWait.h"
#include "workspace/registries/WorkspaceMenuRegistry.h"

#include "TestSupportSdl.h"
#include "support/RetainedRedraw.h"
#include "support/SoftwareCanvas.h"

#include "TerminalSessionTestAccess.h"
#include "render/Theme.h"
#include "perf/AllocationCounter.h"
#include "util/PerformanceCounters.h"
#include "workspace/BreadcrumbLayout.h"
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
#include "support/GitSidebarWait.h"

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using microide::workspace::ActionId;
using microide::workspace::TreeContextTargetKind;
using microide::workspace::WorkspaceTreeContextMenuItems;
using microide::workspace::IsMenuBarTopLevelMenu;

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

#endif

void TestWorkspaceShellMenuBarOmitsRemovedMenus() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1920, 720);

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

void TestWorkspaceShellMenuBarLayoutIsAllocationFree() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);
  const SDL_FRect menu_bar = WorkspaceShellTestAccess::MenuBarRect(shell);
  Expect(WorkspaceShellTestAccess::LayOutMenuBar(shell, menu_bar) > 0,
         "the menu bar should lay out to a non-empty set of items");

  const std::uint64_t measures_before =
      util::ReadPerformanceCounter(util::PerfCounterId::WorkspaceMenuBarLabelMeasures);
  const std::uint64_t layouts_before =
      util::ReadPerformanceCounter(util::PerfCounterId::WorkspaceMenuBarLayouts);
#if MICROIDE_PERF_HARNESS_BUILD
  const microide::tests::perf::AllocationSnapshot before =
      microide::tests::perf::Allocations::Snapshot();
#endif
  std::size_t total = 0;
  for (int i = 0; i < 100; ++i) {
    total += WorkspaceShellTestAccess::LayOutMenuBar(shell, menu_bar);
  }
#if MICROIDE_PERF_HARNESS_BUILD
  const microide::tests::perf::AllocationDelta delta =
      microide::tests::perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0 && delta.bytes_allocated == 0,
         "a warm menu-bar layout must not touch the heap");
#endif
  Expect(total > 0, "the layout results must be observed, not optimised away");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::WorkspaceMenuBarLabelMeasures) ==
             measures_before,
         "a warm menu-bar layout must not re-measure any label width");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::WorkspaceMenuBarLayouts) >
             layouts_before + 100,
         "workspace.menu_bar_layouts must count every layout, including nested ones");
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

void TestWorkspaceShellMenuOverflowPopupFollowsTheWindow() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);
  WorkspaceShellTestAccess::SetWindowSize(shell, 420, 720);
  const auto chevron = WorkspaceShellTestAccess::MenuOverflowChevronRect(shell);
  Expect(chevron.has_value(), "a narrow window overflows the menu bar");
  SDL_Event click{};
  click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  click.button.button = SDL_BUTTON_LEFT;
  click.button.x = chevron->x + chevron->w * 0.5f;
  click.button.y = chevron->y + chevron->h * 0.5f;
  Expect(shell.HandleEvent(click).handled && WorkspaceShellTestAccess::MenuOverflowPopupOpen(shell),
         "the chevron opens the overflow popup");

  WorkspaceShellTestAccess::SetWindowSize(shell, 300, 720);
  const auto popup = WorkspaceShellTestAccess::MenuOverflowPopupRect(shell);
  const auto moved = WorkspaceShellTestAccess::MenuOverflowChevronRect(shell);
  Expect(popup.has_value() && moved.has_value() && popup->x >= 0.0f && popup->x + popup->w <= 300.0f,
         "after the window shrinks the popup is inside it: x " + std::to_string(popup ? popup->x : -1.0f) +
             ", w " + std::to_string(popup ? popup->w : -1.0f));

  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 720);
  Expect(!WorkspaceShellTestAccess::MenuOverflowChevronRect(shell).has_value() &&
             !WorkspaceShellTestAccess::MenuOverflowPopupRect(shell).has_value(),
         "once nothing overflows there is no overflow popup");
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
  Expect(!result.redraw.full && result.redraw.frame,
         "opening the compact menu should owe a frame, not a full repaint");
}

void TestWorkspaceShellCompactMenuOverflowPaintsWhatAFullRepaintPaints() {
  constexpr int kWidth = 640;
  constexpr int kHeight = 480;
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "proj";
  std::filesystem::create_directories(root);
  WriteFile(root / "a.txt", "alpha\nbeta\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);
  WorkspaceShellTestAccess::SetLayoutMode(shell, microide::workspace::LayoutMode::Compact);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "a.txt");
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto chevron = WorkspaceShellTestAccess::MenuOverflowChevronRect(shell);
  Expect(chevron.has_value(), "the compact layout shows the menu chevron");
  const float cx = chevron->x + chevron->w * 0.5f;
  const float cy = chevron->y + chevron->h * 0.5f;
  const float away_x = static_cast<float>(kWidth) * 0.5f;
  const float away_y = static_cast<float>(kHeight) * 0.6f;
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const auto check = [&](const char* label, std::initializer_list<SDL_Event> events) {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                  DispatchEvents(shell, events), pin_caret);
    Expect(diff.identical(), std::string(label) + " should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  };
  check("hovering the chevron", {MouseMotionEvent(cx, cy)});
  check("leaving the chevron", {MouseMotionEvent(away_x, away_y)});
  check("opening the popup", {MouseMotionEvent(cx, cy), MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, cx, cy),
                              MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, cx, cy)});
  Expect(WorkspaceShellTestAccess::MenuOverflowPopupOpen(shell), "the click opened the popup");
  check("Escape closing the popup", {KeyDownEvent(SDLK_ESCAPE)});
  Expect(!WorkspaceShellTestAccess::MenuOverflowPopupOpen(shell), "Escape closed the popup");
  const auto caret_off = [&]() {
    WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks() - 600);
  };
  caret_off();
  {
    const auto diff = PaintAndDiffWithFullRepaint(
        shell, canvas, kWidth, kHeight,
        DispatchEvents(shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, cx, cy),
                               MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, cx, cy)}),
        caret_off);
    Expect(diff.identical(), "opening the popup with the caret off should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  }
  pin_caret();
  check("Escape closing the popup as the caret comes back", {KeyDownEvent(SDLK_ESCAPE)});
  check("opening the popup again", {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, cx, cy),
                                    MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, cx, cy)});
  const auto popup = WorkspaceShellTestAccess::MenuOverflowPopupRect(shell);
  Expect(popup.has_value(), "the open popup has a rect");
  const float row_x = popup->x + popup->w * 0.5f;
  const float row_y = popup->y + 12.0f;
  check("a popup row opening its menu", {MouseMotionEvent(row_x, row_y),
                                         MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, row_x, row_y),
                                         MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, row_x, row_y)});
  Expect(!WorkspaceShellTestAccess::MenuOverflowPopupOpen(shell) && WorkspaceShellTestAccess::MenuBarOpen(shell),
         "the row closed the popup and opened its menu");
  check("Escape closing the menu", {KeyDownEvent(SDLK_ESCAPE)});
  check("opening the popup a third time", {MouseMotionEvent(cx, cy),
                                           MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, cx, cy),
                                           MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, cx, cy)});
  check("a press outside closing the popup", {MouseMotionEvent(away_x, away_y),
                                              MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, away_x, away_y),
                                              MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, away_x, away_y)});
  Expect(!WorkspaceShellTestAccess::MenuOverflowPopupOpen(shell), "the outside press closed the popup");
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

void TestWorkspaceShellCompactMenuOverflowIsKeyboardNavigable() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);
  WorkspaceShellTestAccess::SetLayoutMode(shell, microide::workspace::LayoutMode::Compact);

  const auto chevron = WorkspaceShellTestAccess::MenuOverflowChevronRect(shell);
  Expect(chevron.has_value(), "compact keyboard fixture should expose the overflow button");
  const std::size_t overflow_count = WorkspaceShellTestAccess::MenuBarOverflowItemCount(shell);
  Expect(overflow_count > 1,
         "compact keyboard fixture needs at least two overflow rows to step between");
  Expect(SendMouseDown(shell, chevron->x + chevron->w * 0.5f, chevron->y + chevron->h * 0.5f,
                       SDL_BUTTON_LEFT),
         "compact keyboard fixture should open the overflow popup");
  Expect(WorkspaceShellTestAccess::MenuOverflowPopupActiveIndex(shell) == -1,
         "a freshly opened overflow popup should highlight nothing");

  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE),
         "Down should be consumed by the open overflow popup");
  Expect(WorkspaceShellTestAccess::MenuOverflowPopupActiveIndex(shell) == 0,
         "Down from no selection should land on the first overflow row");
  Expect(SendKeyDown(shell, SDLK_UP, SDL_KMOD_NONE), "Up should be consumed too");
  Expect(WorkspaceShellTestAccess::MenuOverflowPopupActiveIndex(shell) ==
             static_cast<int>(overflow_count) - 1,
         "Up from the first row should wrap to the last");
  Expect(SendKeyDown(shell, SDLK_HOME, SDL_KMOD_NONE), "Home should be consumed");
  Expect(WorkspaceShellTestAccess::MenuOverflowPopupActiveIndex(shell) == 0,
         "Home should jump to the first overflow row");

  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Enter should be consumed by the open overflow popup");
  Expect(!WorkspaceShellTestAccess::MenuOverflowPopupOpen(shell),
         "opening a row from the keyboard should close the overflow popup");
  Expect(WorkspaceShellTestAccess::FileMenuOpen(shell),
         "Enter on the first overflow row should open the File menu, as a click does");
}

void TestWorkspaceShellCompactMenuOverflowEscapeClosesIt() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);
  WorkspaceShellTestAccess::SetLayoutMode(shell, microide::workspace::LayoutMode::Compact);

  const auto chevron = WorkspaceShellTestAccess::MenuOverflowChevronRect(shell);
  Expect(chevron.has_value(), "compact escape fixture should expose the overflow button");
  Expect(SendMouseDown(shell, chevron->x + chevron->w * 0.5f, chevron->y + chevron->h * 0.5f,
                       SDL_BUTTON_LEFT),
         "compact escape fixture should open the overflow popup");
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE),
         "Esc should be consumed by the open overflow popup");
  Expect(!WorkspaceShellTestAccess::MenuOverflowPopupOpen(shell),
         "Esc should close the overflow popup");
  Expect(WorkspaceShellTestAccess::MenuOverflowPopupActiveIndex(shell) == -1,
         "closing the overflow popup should clear its highlighted row");
}

void TestWorkspaceShellCompactMenuOverflowMotionTracksHoveredRow() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);
  WorkspaceShellTestAccess::SetLayoutMode(shell, microide::workspace::LayoutMode::Compact);

  const auto chevron = WorkspaceShellTestAccess::MenuOverflowChevronRect(shell);
  Expect(chevron.has_value(), "compact motion fixture should expose the overflow button");
  Expect(SendMouseDown(shell, chevron->x + chevron->w * 0.5f, chevron->y + chevron->h * 0.5f,
                       SDL_BUTTON_LEFT),
         "compact motion fixture should open the overflow popup");
  const auto popup = WorkspaceShellTestAccess::MenuOverflowPopupRect(shell);
  Expect(popup.has_value(), "compact motion fixture should expose the popup rect");

  const float row_height = microide::workspace::kWorkspaceMenuPopupItemHeight;
  Expect(SendMouseMotion(shell, popup->x + popup->w * 0.5f, popup->y + 4.0f + row_height * 1.5f, 0),
         "a motion inside the overflow popup should be consumed by it");
  Expect(WorkspaceShellTestAccess::MenuOverflowPopupActiveIndex(shell) == 1,
         "a motion over the second overflow row should highlight it");
  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE), "Down should be consumed");
  Expect(WorkspaceShellTestAccess::MenuOverflowPopupActiveIndex(shell) == 2,
         "the keyboard should step on from wherever the pointer left the highlight");
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

  const float empty_x = 1500.0f;
  Expect(shell.WindowHitTest(empty_x, 10.0f) == SDL_HITTEST_DRAGGABLE,
         "empty title-bar hit testing should hand borderless dragging back to the window manager");
  Expect(shell.WindowDragRegionContains(empty_x, 10.0f),
         "empty title-bar space should still be eligible for window dragging");
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

void TestWorkspaceShellMenuEventsOweAFrame() {
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
  Expect(open_result.handled, "opening a menu should be handled");
  Expect(!open_result.redraw.full && open_result.redraw.frame && open_result.redraw.rects.empty(),
         "opening a menu should owe a frame and name no rect");

  SDL_Event motion_event{};
  motion_event.type = SDL_EVENT_MOUSE_MOTION;
  motion_event.motion.x = edit_rect->x + edit_rect->w * 0.5f;
  motion_event.motion.y = edit_rect->y + edit_rect->h * 0.5f;
  const auto hover_result = shell.HandleEvent(motion_event);
  Expect(hover_result.handled, "switching menu hover should be handled");
  Expect(!hover_result.redraw.full && hover_result.redraw.frame && hover_result.redraw.rects.empty(),
         "switching menu hover should owe a frame and name no rect");
}

void TestWorkspaceShellPopupRowHoverOwesAFrame() {
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
  Expect(!hover_result.redraw.full && hover_result.redraw.frame && hover_result.redraw.rects.empty(),
         "popup row hover should owe a frame and name no rect of its own");
}

struct HoverRepaint {
  bool handled = false;
  bool owes_frame = false;
  microide::workspace::RenderInvalidation damage;
};
HoverRepaint PaintThenMovePointer(WorkspaceShell& shell, float x, float y) {
  SoftwareCanvas canvas{1280, 720};
  shell.Render(canvas.renderer(), 1280, 720);
  (void)shell.ConsumePendingRenderInvalidation();
  SDL_Event motion_event{};
  motion_event.type = SDL_EVENT_MOUSE_MOTION;
  motion_event.motion.x = x;
  motion_event.motion.y = y;
  const auto result = shell.HandleEvent(motion_event);
  return HoverRepaint{.handled = result.handled,
                      .owes_frame = result.redraw.frame || result.redraw.HasDamage(),
                      .damage = FrameDamage(shell, canvas, 1280, 720, result.redraw)};
}

float DamagedArea(const microide::workspace::RenderInvalidation& damage) {
  float area = 0.0f;
  for (const SDL_FRect& rect : damage.rects) {
    area += rect.w * rect.h;
  }
  return area;
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
  const auto departed_tooltip = WorkspaceShellTestAccess::HoveredTooltipRect(shell);
  Expect(departed_tooltip.has_value(),
         "an icon-only sidebar mode tab should name itself on hover");

  const HoverRepaint hover = PaintThenMovePointer(shell, collapse_rect.x + collapse_rect.w * 0.5f,
                                                  collapse_rect.y + collapse_rect.h * 0.5f);
  Expect(hover.handled && hover.owes_frame, "switching tree header hover should be handled and owe a frame");
  Expect(!hover.damage.full && !hover.damage.rects.empty(), "tree header hover should stay on a partial redraw path");
  Expect(AnyRectCovers(hover.damage.rects, mode_rect), "tree header hover should redraw the previously hovered control");
  Expect(AnyRectCovers(hover.damage.rects, collapse_rect), "tree header hover should redraw the newly hovered control");
  Expect(AnyRectCovers(hover.damage.rects, *departed_tooltip),
         "leaving a mode tab should repaint the tooltip card it was showing");
  Expect(DamagedArea(hover.damage) < 0.05f * 1280.0f * 720.0f,
         "tree header hover should only repaint the affected controls");
}

void TestWorkspaceShellSearchSidebarHeaderHoverReturnsButtonOnlyInvalidation() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "main.cpp", "int main() {}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "main", false);
  WaitForProjectSearch(shell);

  const SDL_FRect mode_rect = WorkspaceShellTestAccess::SearchSidebarModeButtonRect(shell);
  const SDL_FRect case_rect = WorkspaceShellTestAccess::SearchSidebarCaseButtonRect(shell);
  Expect(mode_rect.w > 0.0f && case_rect.w > 0.0f,
         "search hover fixture should expose the mode and case buttons");

  Expect(SendMouseMotion(shell, mode_rect.x + mode_rect.w * 0.5f,
                         mode_rect.y + mode_rect.h * 0.5f, 0),
         "priming the search mode hover should be handled");

  const HoverRepaint hover =
      PaintThenMovePointer(shell, case_rect.x + case_rect.w * 0.5f, case_rect.y + case_rect.h * 0.5f);
  Expect(hover.handled && hover.owes_frame, "switching search header hover should be handled and owe a frame");
  Expect(!hover.damage.full && !hover.damage.rects.empty(), "search header hover should stay on a partial redraw path");
  Expect(AnyRectCovers(hover.damage.rects, mode_rect),
         "search header hover should redraw the previously hovered control");
  Expect(AnyRectCovers(hover.damage.rects, case_rect), "search header hover should redraw the newly hovered control");
  Expect(DamagedArea(hover.damage) < 0.05f * 1280.0f * 720.0f,
         "search header hover should only repaint the affected controls");
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

  const HoverRepaint hover = PaintThenMovePointer(shell, top_action_rects[2].x + top_action_rects[2].w * 0.5f,
                                                  top_action_rects[2].y + top_action_rects[2].h * 0.5f);
  Expect(hover.handled && hover.owes_frame, "switching git header hover should be handled and owe a frame");
  Expect(!hover.damage.full && !hover.damage.rects.empty(), "git header hover should stay on a partial redraw path");
  Expect(DamagedArea(hover.damage) < 0.05f * 1280.0f * 720.0f,
         "git header hover should only repaint the affected controls");
  Expect(AnyRectCovers(hover.damage.rects, top_action_rects[0]),
         "git header hover should redraw the previously hovered control");
  Expect(AnyRectCovers(hover.damage.rects, top_action_rects[2]),
         "git header hover should redraw the newly hovered control");
  const auto arrived_tooltip = WorkspaceShellTestAccess::HoveredTooltipRect(shell);
  Expect(arrived_tooltip.has_value(), "hovering git Refresh should resolve a tooltip");
  Expect(AnyRectCovers(hover.damage.rects, *arrived_tooltip),
         "arriving on git Refresh should repaint its tooltip card");
}

void TestWorkspaceShellGitSidebarHeaderButtonsHaveTooltips() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add git tooltip fixture", "git tooltip fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(WaitForGitSidebarEntryCount(shell, 1),
         "git tooltip fixture should expose one changed row");

  const SDL_FRect sync = WorkspaceShellTestAccess::GitSidebarSyncButtonRect(shell);
  Expect(sync.w > 0.0f, "git tooltip fixture should expose the Sync button");
  Expect(SendMouseMotion(shell, sync.x + sync.w * 0.5f, sync.y + sync.h * 0.5f, 0),
         "hovering the git Sync button should be handled");
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell).find("upstream") !=
             std::string::npos,
         "hovering Sync with no upstream should say so");

  const auto top_action_rects = WorkspaceShellTestAccess::GitSidebarTopActionRects(shell);
  Expect(SendMouseMotion(shell, top_action_rects[0].x + top_action_rects[0].w * 0.5f,
                         top_action_rects[0].y + top_action_rects[0].h * 0.5f, 0),
         "hovering the git Stage All button should be handled");
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell) == "Stage All Changes",
         "hovering Stage All should name what it does");
  Expect(SendMouseMotion(shell, top_action_rects[1].x + top_action_rects[1].w * 0.5f,
                         top_action_rects[1].y + top_action_rects[1].h * 0.5f, 0),
         "hovering the git Discard All button should be handled");
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell) == "Discard All Changes",
         "hovering Discard All should name what it does");
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
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell) == "src/deep/main.cpp",
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
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell).empty(),
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
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell) == "src/deep/main.cpp",
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
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell).empty(),
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
  project.lsp.request_in_flight_count = 1;
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

  Expect(!shell.CurrentCaretDirtyRect().has_value(), "a focused editor's blink names no rect of its own");
  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks());
  (void)shell.ConsumePendingRenderInvalidation();
  const WorkspaceShell::EventResult wake = WorkspaceShellTestAccess::HandleScheduledWake(shell);
  Expect(wake.handled && wake.redraw.frame && !wake.redraw.full && wake.redraw.rects.empty(),
         "a focused editor's blink wake asks for a frame and names no rect");
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

  SoftwareCanvas canvas{1280, 720};
  shell.Render(canvas.renderer(), 1280, 720);
  (void)shell.ConsumePendingRenderInvalidation();
  SDL_Event event{};
  event.type = SDL_EVENT_TEXT_INPUT;
  const std::string text = "x";
  event.text.text = text.c_str();
  const auto result = shell.HandleEvent(event);
  const auto damage = FrameDamage(shell, canvas, 1280, 720, result.redraw);
  const SDL_FRect active_pane = WorkspaceShellTestAccess::ActiveEditorPaneRect(shell);
  const auto edited_line_rect = WorkspaceShellTestAccess::ActiveEditorLineRangeRect(shell, 0, 1);

  Expect(result.handled, "editor typing should be handled");
  Expect(!damage.full && !damage.rects.empty(),
         "editor typing should request a partial redraw");
  Expect(AnyRectIntersects(damage.rects, active_pane),
         "editor typing redraws should include the active editor pane");
  Expect(edited_line_rect.has_value() && AnyRectIntersects(damage.rects, *edited_line_rect),
         "editor typing redraws should include the edited line band");
  std::string rects;
  for (const SDL_FRect& r : damage.rects) {
    rects += " [" + std::to_string(static_cast<int>(r.x)) + "," + std::to_string(static_cast<int>(r.y)) +
             " " + std::to_string(static_cast<int>(r.w)) + "x" + std::to_string(static_cast<int>(r.h)) + "]";
  }
  Expect(InPaneDamageHeight(damage.rects, active_pane) < active_pane.h,
         "single-line editor typing should redraw less than the full active pane;" + rects);
}

void TestWorkspaceShellActionWithNoTreeMenuOpenLeavesTheChromeAlone() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "left.cpp";
  WriteFile(source, "alpha beta\ngamma\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  (void)shell.ConsumePendingRenderInvalidation();

  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::ExpandSelection,
                                                 {}, WorkspaceShell::ActionSource::Shortcut),
         "expand-selection should run");
  const auto invalidation = shell.ConsumePendingRenderInvalidation();
  const SDL_FRect menu_bar = WorkspaceShellTestAccess::CurrentLayout(shell).menu_bar;
  Expect(!invalidation.full && !AnyRectIntersects(invalidation.rects, menu_bar),
         "an action with no tree context menu open should not repaint the menu bar");
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

  SoftwareCanvas canvas{700, 720};
  shell.Render(canvas.renderer(), 700, 720);
  (void)shell.ConsumePendingRenderInvalidation();
  SDL_Event event{};
  event.type = SDL_EVENT_TEXT_INPUT;
  const std::string text = "!";
  event.text.text = text.c_str();
  const auto result = shell.HandleEvent(event);
  const auto damage = FrameDamage(shell, canvas, 700, 720, result.redraw);
  const SDL_FRect active_pane = WorkspaceShellTestAccess::ActiveEditorPaneRect(shell);

  Expect(result.handled, "wrapped typing should be handled");
  Expect(!damage.full && !damage.rects.empty(),
         "wrapped typing should stay on the partial redraw path");
  Expect(AnyRectIntersects(damage.rects, active_pane),
         "wrapped typing redraws should include the active editor pane");
  const auto edited_row = WorkspaceShellTestAccess::ActiveEditorLineRangeRect(shell, 0, 1);
  Expect(edited_row.has_value() && AnyRectIntersects(damage.rects, *edited_row),
         "wrapped typing should repaint the edited line's rows");
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
  workspace::RenderInvalidation asked = result.redraw;
  asked.Merge(shell.ConsumePendingRenderInvalidation());
  Expect(!asked.full && (asked.frame || !asked.rects.empty()),
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

void TestWorkspaceShellCommandPaletteRanksExactCommandNameFirst() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(SendKeyDown(shell, SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "palette ranking fixture should open the command palette");

  WorkspaceShellTestAccess::SetCommandPaletteQueryAndRefresh(shell, "open");
  Expect(WorkspaceShellTestAccess::CommandPaletteMatchCount(shell) > 1,
         "'open' should match more than one command");
  Expect(WorkspaceShellTestAccess::CommandPaletteMatchLabelAt(shell, 0) == "Open File…",
         "the command actually named 'open' should rank first");

  WorkspaceShellTestAccess::SetCommandPaletteQueryAndRefresh(shell, "search");
  Expect(WorkspaceShellTestAccess::CommandPaletteMatchLabelAt(shell, 0) == "Find in Buffer",
         "the command actually named 'search' should rank first");

  WorkspaceShellTestAccess::SetCommandPaletteQueryAndRefresh(shell, "goto-t");
  Expect(WorkspaceShellTestAccess::CommandPaletteMatchLabelAt(shell, 0) == "Go to Type Definition",
         "a command-name prefix should rank ahead of an incidental substring match");
}

void TestWorkspaceShellCommandPaletteMatchesQueryWordsInAnyOrder() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(SendKeyDown(shell, SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "palette fixture should open the command palette");

  WorkspaceShellTestAccess::SetCommandPaletteQueryAndRefresh(shell, "line move");
  const std::size_t reordered = WorkspaceShellTestAccess::CommandPaletteMatchCount(shell);
  Expect(reordered >= 2, "'line move' matches the move-line commands although the words are swapped");
  bool found_move_line_down = false;
  for (std::size_t i = 0; i < reordered; ++i) {
    if (WorkspaceShellTestAccess::CommandPaletteMatchLabelAt(shell, i) == "Move Line Down") {
      found_move_line_down = true;
    }
  }
  Expect(found_move_line_down, "'line move' lists Move Line Down");

  WorkspaceShellTestAccess::SetCommandPaletteQueryAndRefresh(shell, "move line");
  Expect(WorkspaceShellTestAccess::CommandPaletteMatchCount(shell) >= reordered,
         "the same words in row order match at least as many rows");
  Expect(WorkspaceShellTestAccess::CommandPaletteMatchLabelAt(shell, 0).rfind("Move Line", 0) == 0,
         "a contiguous run of the query ranks first: " +
             WorkspaceShellTestAccess::CommandPaletteMatchLabelAt(shell, 0));

  WorkspaceShellTestAccess::SetCommandPaletteQueryAndRefresh(shell, "zz-no-such-word line");
  Expect(WorkspaceShellTestAccess::CommandPaletteMatchCount(shell) == 0,
         "every query word has to appear somewhere in the row");
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

  const SDL_FRect tree_tab = WorkspaceShellTestAccess::SidebarModeTabRect(shell, "tree");
  const SDL_FRect search_tab = WorkspaceShellTestAccess::SidebarModeTabRect(shell, "search");
  const SDL_FRect git_tab = WorkspaceShellTestAccess::SidebarModeTabRect(shell, "git");
  Expect(tree_tab.w > 0.0f && search_tab.w > 0.0f && git_tab.w > 0.0f,
         "Project, Search, and Source Control tabs should all be present");
  Expect(tree_tab.x < search_tab.x && search_tab.x < git_tab.x,
         "mode tabs should be laid out in Project / Search / Source Control order");
  Expect(WorkspaceShellTestAccess::SidebarModeOverflowRect(shell).w > 0.0f,
         "the tabless builtin views should give the row an overflow button");
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
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell) == "src/deep/main.cpp",
         "tooltip layering fixture should produce the hovered tab tooltip");

  const auto tooltip_rect = WorkspaceShellTestAccess::HoveredTooltipRect(shell);
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

void TestWorkspaceShellRenderedFrameIsFullyOpaque() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 900, 600);
  WorkspaceShellTestAccess::OpenCommandPalette(shell);
  Expect(WorkspaceShellTestAccess::CommandPaletteOpen(shell),
         "the opacity fixture needs a backdrop-drawing modal open");

  const auto theme = microide::render::MakeDefaultTheme();
  Expect(theme.overlay_backdrop.a != SDL_ALPHA_OPAQUE,
         "the fixture is only meaningful while the modal backdrop is translucent");

  SoftwareCanvas canvas(900, 600);
  shell.Render(canvas.renderer(), 900, 600);

  int translucent_pixels = 0;
  SDL_Color first_bad{};
  int first_bad_x = 0;
  int first_bad_y = 0;
  for (int y = 0; y < 600; ++y) {
    for (int x = 0; x < 900; ++x) {
      const SDL_Color pixel = canvas.PixelAt(x, y);
      if (pixel.a != SDL_ALPHA_OPAQUE) {
        if (translucent_pixels == 0) {
          first_bad = pixel;
          first_bad_x = x;
          first_bad_y = y;
        }
        ++translucent_pixels;
      }
    }
  }
  Expect(translucent_pixels == 0,
         "a painted frame must be fully opaque; " + std::to_string(translucent_pixels) +
             " pixels were not, first at (" + std::to_string(first_bad_x) + "," +
             std::to_string(first_bad_y) + ") alpha=" + std::to_string(first_bad.a));

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const SDL_Color dimmed =
      canvas.PixelAt(static_cast<int>(layout.editor_surface.x + 8.0f),
                     static_cast<int>(layout.editor_surface.y + layout.editor_surface.h - 8.0f));
  Expect(!(dimmed.r == theme.overlay_backdrop.r && dimmed.g == theme.overlay_backdrop.g &&
           dimmed.b == theme.overlay_backdrop.b),
         "the modal backdrop should dim the editor behind it, not overwrite it");
}

void TestWorkspaceShellStatusBarColumnIsTheVisibleColumn() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "\tab\n\xc3\xa9" "a\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.SetTabSize(4);
  using microide::workspace::StatusBarSegmentId;

  editor.MoveCursorTo(0, 1);
  WorkspaceShellTestAccess::RefreshStatusBar(shell);
  Expect(WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::LineColumn) ==
             "Ln 1, Col 5",
         "a tab counts as its width: " +
             WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::LineColumn));

  editor.MoveCursorTo(1, 2);
  WorkspaceShellTestAccess::RefreshStatusBar(shell);
  Expect(WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::LineColumn) ==
             "Ln 2, Col 2",
         "a multibyte character counts as one column: " +
             WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::LineColumn));

  editor.MoveCursorTo(0, 1);
  editor.ReplaceRange(microide::editor::SelectionRange{{0, 0}, {0, 1}}, " ");
  editor.MoveCursorTo(0, 1);
  WorkspaceShellTestAccess::RefreshStatusBar(shell);
  Expect(WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::LineColumn) ==
             "Ln 1, Col 2",
         "the column follows the bytes before the caret, not just the caret");
}

void TestWorkspaceShellStatusBarHidesEditorSegmentsWithNoEditorOpen() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README", "hello\n");
  using microide::workspace::StatusBarSegmentId;
  constexpr StatusBarSegmentId kEditorSegments[] = {
      StatusBarSegmentId::LineColumn, StatusBarSegmentId::Indent,
      StatusBarSegmentId::Language, StatusBarSegmentId::Encoding};

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::RefreshStatusBar(shell);
  for (const StatusBarSegmentId id : kEditorSegments) {
    Expect(!WorkspaceShellTestAccess::StatusBarSegmentVisible(shell, id),
           "no editor open: segment should be hidden, shows '" +
               WorkspaceShellTestAccess::StatusBarSegmentText(shell, id) + "'");
  }

  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::Tab, {}),
         "a new untitled tab should open");
  WorkspaceShellTestAccess::RefreshStatusBar(shell);
  for (const StatusBarSegmentId id : kEditorSegments) {
    Expect(WorkspaceShellTestAccess::StatusBarSegmentVisible(shell, id),
           "an untitled buffer is an editor: its segments stay visible");
  }
  Expect(WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::Language) ==
             "Plain Text",
         "the fallback language reads Plain Text, got '" +
             WorkspaceShellTestAccess::StatusBarSegmentText(shell, StatusBarSegmentId::Language) +
             "'");
}

void TestWorkspaceShellStatusBarRepaintsWhenItsValuesChange() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetWindowSize(shell, 900, 600);

  SoftwareCanvas canvas(900, 600);
  shell.Render(canvas.renderer(), 900, 600);
  (void)shell.ConsumePendingRenderInvalidation();

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  Expect(layout.status_bar.w > 0.0f && layout.status_bar.h > 0.0f,
         "the status bar fixture needs a visible status bar");

  shell.Render(canvas.renderer(), 900, 600);
  const auto idle_redraw = shell.ConsumePendingRenderInvalidation();
  Expect(!AnyRectCovers(idle_redraw.rects, layout.status_bar),
         "an unchanged status bar should not ask to be repainted");

  const auto key_result = SendKeyDownResult(shell, SDLK_DOWN, SDL_KMOD_NONE);
  Expect(key_result.handled, "the fixture's caret move should be handled");
  Expect(!key_result.redraw.full && !AnyRectCovers(key_result.redraw.rects, layout.status_bar),
         "the fixture is only meaningful if the keystroke itself misses the strip");

  shell.Render(canvas.renderer(), 900, 600);
  Expect(shell.CurrentIdleWaitState().hint != WorkspaceShell::IdleHint::Idle,
         "the event loop must not block while a render-requested redraw is owed");
  const auto after_move = shell.ConsumePendingRenderInvalidation();
  const std::optional<SDL_FRect> line_column =
      WorkspaceShellTestAccess::StatusBarSegmentRect(shell, microide::workspace::StatusBarSegmentId::LineColumn);
  Expect(line_column.has_value(), "the fixture shows an Ln/Col segment");
  Expect(after_move.full || (line_column.has_value() && AnyRectCovers(after_move.rects, *line_column)),
         "a caret move should repaint the status bar's Ln/Col segment so it follows the caret");
  Expect(after_move.full || !AnyRectCovers(after_move.rects, layout.status_bar),
         "and only the segment that changed, not the whole strip");
}

void TestWorkspaceShellSettingsAndHelpDimTheEditorBehindThem() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  const auto theme = microide::render::MakeDefaultTheme();
  Expect(theme.overlay_backdrop.a != SDL_ALPHA_OPAQUE,
         "the fixture is only meaningful while the backdrop is translucent");

  const auto sample_editor_area = [&](bool help_about) {
    WorkspaceShell shell;
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::OpenFile(shell, source);
    WorkspaceShellTestAccess::SetWindowSize(shell, 900, 600);

    SoftwareCanvas canvas(900, 600);
    shell.Render(canvas.renderer(), 900, 600);
    const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
    const int x = static_cast<int>(layout.editor_area.x + 4.0f);
    const int y = static_cast<int>(layout.editor_area.y + layout.editor_area.h - 6.0f);
    const SDL_Color before = canvas.PixelAt(x, y);

    if (help_about) {
      WorkspaceShellTestAccess::OpenHelpAboutOverlay(shell);
    } else {
      WorkspaceShellTestAccess::OpenSettingsOverlay(shell);
    }
    Expect(WorkspaceShellTestAccess::SettingsOverlayVisible(shell),
           "the fixture should open the settings/help surface");
    shell.Render(canvas.renderer(), 900, 600);
    const SDL_Color after = canvas.PixelAt(x, y);

    Expect(after.a == SDL_ALPHA_OPAQUE,
           "the backdrop must leave the frame opaque, not stamp its own alpha");
    Expect(after.r != before.r || after.g != before.g || after.b != before.b,
           help_about ? "Help/About should dim the editor behind it"
                      : "Settings should dim the editor behind it");
    Expect(!(after.r == theme.overlay_backdrop.r && after.g == theme.overlay_backdrop.g &&
             after.b == theme.overlay_backdrop.b),
           "the backdrop should composite over the editor, not replace it");
  };

  sample_editor_area(false);
  sample_editor_area(true);
}

void TestWorkspaceShellEditorLineRangeDirtyRectCoversItsLines() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "one\ntwo\nthree\nfour\nfive\nsix\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  const auto metrics = WorkspaceShellTestAccess::ActiveEditorRenderMetrics(shell);
  Expect(metrics.line_height > 0.0f, "the fixture should resolve editor render metrics");

  for (const std::size_t span : {std::size_t{1}, std::size_t{3}}) {
    const std::size_t start_line = 1;
    const auto rect =
        WorkspaceShellTestAccess::ActiveEditorLineRangeRect(shell, start_line, start_line + span);
    Expect(rect.has_value(), "a visible line range should resolve a dirty rect");
    const float content_top =
        metrics.first_line_y + static_cast<float>(start_line) * metrics.line_height;
    const float content_bottom = content_top + static_cast<float>(span) * metrics.line_height;
    Expect(rect->y <= content_top,
           "a line-range dirty rect should start at or above its first row");
    Expect(rect->y + rect->h >= content_bottom,
           "a line-range dirty rect should extend past the bottom of its last row");
  }
}

void TestWorkspaceShellFindWidgetHoverRepaintsTheCard() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "alpha\nbeta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "search alpha"),
         "the hover fixture should open the in-file find surface");

  const microide::workspace::FindWidgetLayout fw =
      WorkspaceShellTestAccess::FindWidgetControls(shell, false);
  Expect(SendMouseMotion(shell, fw.search_field.x + 4.0f,
                         fw.search_field.y + fw.search_field.h * 0.5f, 0),
         "priming the find widget hover should be handled");

  const HoverRepaint hover = PaintThenMovePointer(shell, fw.toggle_buttons[0].x + fw.toggle_buttons[0].w * 0.5f,
                                                  fw.toggle_buttons[0].y + fw.toggle_buttons[0].h * 0.5f);
  Expect(hover.handled && hover.owes_frame, "moving onto a find widget button should be handled and owe a frame");
  Expect(!hover.damage.full && !hover.damage.rects.empty(), "find widget hover should stay on a partial redraw path");
  Expect(AnyRectCovers(hover.damage.rects, fw.toggle_buttons[0]),
         "entering a find widget button should repaint the card it is on");
}

void TestWorkspaceShellStatusBarSegmentsExplainThemselvesOnHover() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::RefreshStatusBar(shell);

  const auto line_col_rect =
      WorkspaceShellTestAccess::StatusBarSegmentRect(shell, microide::workspace::StatusBarSegmentId::LineColumn);
  Expect(line_col_rect.has_value(), "the status bar should place a line/column segment");
  (void)SendMouseMotion(shell, line_col_rect->x + line_col_rect->w * 0.5f,
                        line_col_rect->y + line_col_rect->h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell) == "Go to line/column",
         "hovering the line/column segment should show what clicking it does");

  const auto tooltip_rect = WorkspaceShellTestAccess::HoveredTooltipRect(shell);
  Expect(tooltip_rect.has_value(), "a status bar tooltip should resolve a card rect");
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  Expect(tooltip_rect->y + tooltip_rect->h <= layout.status_bar.y,
         "a status bar tooltip should flip above the bar instead of off-screen");
  Expect(tooltip_rect->y >= layout.full.y,
         "a flipped tooltip should stay inside the window");

#if MICROIDE_HAS_SDL3_TTF
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "the status bar tooltip fixture should capture rendered pixels");
  const auto theme = microide::render::MakeDefaultTheme();
  const SDL_Color actual =
      ReadSurfacePixelOrThrow(pixels, static_cast<int>(std::floor(tooltip_rect->x + 2.0f)),
                              static_cast<int>(std::floor(tooltip_rect->y + 2.0f)));
  Expect(actual.r == theme.surface_raised.r && actual.g == theme.surface_raised.g &&
             actual.b == theme.surface_raised.b && actual.a == theme.surface_raised.a,
         "a status bar tooltip should render as the shared compact tooltip card");
  SDL_DestroySurface(pixels);
#endif
}

void TestWorkspaceShellFindWidgetControlsNameThemselvesOnHover() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "alpha\nbeta\nalpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "search alpha"),
         "the find widget fixture should open the in-file find surface");

  const microide::workspace::FindWidgetLayout fw = WorkspaceShellTestAccess::FindWidgetControls(shell, false);
  Expect(fw.toggle_count == microide::workspace::kBufferFindToggleCount,
         "the in-file find widget should expose all three option toggles");

  const auto hover = [&](const SDL_FRect& rect) {
    (void)SendMouseMotion(shell, rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f, 0);
    return WorkspaceShellTestAccess::HoveredTooltipLabel(shell);
  };
  Expect(hover(fw.toggle_buttons[0]) == "Match Case (Alt+C)",
         "the Aa toggle should name itself and its chord");
  Expect(hover(fw.toggle_buttons[1]) == "Match Whole Word (Alt+W)",
         "the ab toggle should name itself and its chord");
  Expect(hover(fw.toggle_buttons[2]) == "Use Regular Expression (Alt+R)",
         "the .* toggle should name itself and its chord");
  Expect(hover(fw.next_button) == "Next Match (Enter)",
         "the next-match button should name itself and its chord");
  Expect(hover(fw.prev_button) == "Previous Match (Shift+Enter)",
         "the previous-match button should name itself and its chord");
  Expect(hover(fw.close_button) == "Close (Escape)",
         "the find widget close button should name itself and its chord");
}

void TestWorkspaceShellTooltipCardIsAnchoredToItsControl() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "deep" / "main.cpp";
  WriteFile(source, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  const SDL_FRect tab_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  (void)SendMouseMotion(shell, tab_rect.x + tab_rect.w * 0.25f,
                        tab_rect.y + tab_rect.h * 0.5f, 0);
  const auto left_hover = WorkspaceShellTestAccess::HoveredTooltipRect(shell);
  (void)SendMouseMotion(shell, tab_rect.x + tab_rect.w * 0.75f,
                        tab_rect.y + tab_rect.h * 0.5f, 0);
  const auto right_hover = WorkspaceShellTestAccess::HoveredTooltipRect(shell);
  Expect(left_hover.has_value() && right_hover.has_value(),
         "both ends of a hovered tab should resolve a tooltip");
  Expect(left_hover->x == right_hover->x && left_hover->y == right_hover->y,
         "a tooltip should not follow the pointer inside one control");
  Expect(left_hover->y >= tab_rect.y + tab_rect.h,
         "a tab tooltip should sit below the tab it names");
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
  (void)SettleGitSidebarRefresh(shell);

  const auto top_action_rects = WorkspaceShellTestAccess::GitSidebarTopActionRects(shell);
  (void)SendMouseMotion(
      shell, top_action_rects[2].x + top_action_rects[2].w * 0.5f,
      top_action_rects[2].y + top_action_rects[2].h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredTooltipLabel(shell) == "Refresh",
         "git tooltip fixture should expose the compact action tooltip");

  const auto tooltip_rect = WorkspaceShellTestAccess::HoveredTooltipRect(shell);
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

void TestWorkspaceShellOpeningAMenuByClickKeepsTheSidebar() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  for (const char* name : {"alpha.cpp", "beta.cpp", "gamma.cpp", "README.md"}) {
    WriteFile(root / name, "int value = 1;\n");
  }

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the menu click fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)WorkspaceShellTestAccess::TakePendingRenderInvalidation(shell);

  const std::optional<SDL_FRect> file_item =
      WorkspaceShellTestAccess::MenuBarItemRect(shell, WorkspaceShell::MenuId::File);
  Expect(file_item.has_value(), "the File menu should be on the menu bar");
  const float x = file_item->x + file_item->w / 2.0f;
  const float y = file_item->y + file_item->h / 2.0f;
  microide::workspace::RenderInvalidation invalidation;
  for (const SDL_Event& event : {MouseMotionEvent(x, y), MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                                 MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)}) {
    invalidation.Merge(shell.HandleEvent(event).redraw);
  }
  invalidation.Merge(WorkspaceShellTestAccess::TakePendingRenderInvalidation(shell));
  Expect(WorkspaceShellTestAccess::MenuBarOpen(shell), "the click should open the File menu");
  Expect(invalidation.HasDamage() || invalidation.frame, "opening a menu must ask for a frame");

  pin_caret();
  PaintInvalidation(shell, canvas, kWidth, kHeight, invalidation);
  SDL_FlushRenderer(canvas.renderer());
  SDL_Surface* retained = SDL_DuplicateSurface(canvas.surface());
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  SDL_FlushRenderer(canvas.renderer());
  const std::size_t differences = CountPixelDifferences(retained, canvas.surface());
  SDL_DestroySurface(retained);
  Expect(differences == 0,
         "opening a menu by click should paint what a full repaint paints; " +
             std::to_string(differences) + " pixels differ");
}

void TestWorkspaceShellHoveringIntoAClickedMenuRepaintsItsFirstRow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the menu hover fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)WorkspaceShellTestAccess::TakePendingRenderInvalidation(shell);

  const std::optional<SDL_FRect> file_item =
      WorkspaceShellTestAccess::MenuBarItemRect(shell, WorkspaceShell::MenuId::File);
  Expect(file_item.has_value(), "the File menu should be on the menu bar");
  const float bar_x = file_item->x + file_item->w / 2.0f;
  const float bar_y = file_item->y + file_item->h / 2.0f;
  microide::workspace::RenderInvalidation invalidation;
  for (const SDL_Event& event :
       {MouseMotionEvent(bar_x, bar_y), MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, bar_x, bar_y),
        MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, bar_x, bar_y)}) {
    invalidation.Merge(shell.HandleEvent(event).redraw);
  }
  invalidation.Merge(WorkspaceShellTestAccess::TakePendingRenderInvalidation(shell));
  pin_caret();
  PaintInvalidation(shell, canvas, kWidth, kHeight, invalidation);

  const std::optional<SDL_FRect> second_row = WorkspaceShellTestAccess::PopupMenuItemRect(
      shell, WorkspaceShell::MenuId::File, "Open File\u2026");
  Expect(second_row.has_value(), "the File menu should list Open File");
  invalidation = {};
  invalidation.Merge(
      shell.HandleEvent(MouseMotionEvent(second_row->x + 20.0f, second_row->y + second_row->h / 2.0f))
          .redraw);
  invalidation.Merge(WorkspaceShellTestAccess::TakePendingRenderInvalidation(shell));
  pin_caret();
  PaintInvalidation(shell, canvas, kWidth, kHeight, invalidation);
  SDL_FlushRenderer(canvas.renderer());
  SDL_Surface* retained = SDL_DuplicateSurface(canvas.surface());
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  SDL_FlushRenderer(canvas.renderer());
  const std::size_t differences = CountPixelDifferences(retained, canvas.surface());
  SDL_DestroySurface(retained);
  Expect(differences == 0, "hovering into a clicked menu should paint what a full repaint paints; " +
                               std::to_string(differences) + " pixels differ");
}

void TestWorkspaceShellEscapeFromFindInFilesRepaintsTheField() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the find-in-files fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const auto open = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight,
      DispatchEvents(shell, {KeyDownEvent(SDLK_F, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT))}),
      pin_caret);
  Expect(open.identical(), "Ctrl+Shift+F should paint what a full repaint paints; " +
                               DescribePixelDiff(open));
  const auto escape = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight, DispatchEvents(shell, {KeyDownEvent(SDLK_ESCAPE)}), pin_caret);
  Expect(escape.identical(), "Escape from find-in-files should paint what a full repaint paints; " +
                                 DescribePixelDiff(escape));
}

void TestWorkspaceShellRowUnderAnOpenMenuDoesNotHoverThroughIt() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  for (const char* name : {"alpha.cpp", "beta.cpp", "gamma.cpp", "delta.cpp", "epsilon.cpp",
                           "zeta.cpp", "eta.cpp", "theta.cpp"}) {
    WriteFile(root / name, "int value = 1;\n");
  }

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the menu hover fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const std::optional<SDL_FRect> edit = WorkspaceShellTestAccess::MenuBarItemRect(shell, WorkspaceShell::MenuId::Edit);
  Expect(edit.has_value(), "the Edit menu should be on the menu bar");
  const float bar_x = edit->x + edit->w / 2.0f;
  const float bar_y = edit->y + edit->h / 2.0f;
  const auto open = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight,
      DispatchEvents(shell, {MouseMotionEvent(bar_x, bar_y),
                             MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, bar_x, bar_y),
                             MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, bar_x, bar_y)}),
      pin_caret);
  Expect(open.identical(), "opening the Edit menu should paint what a full repaint paints; " +
                               DescribePixelDiff(open));

  const std::optional<SDL_FRect> row =
      WorkspaceShellTestAccess::PopupMenuItemRect(shell, WorkspaceShell::MenuId::Edit, "Paste");
  Expect(row.has_value(), "the Edit menu should list Paste");
  const SDL_FRect sidebar = WorkspaceShellTestAccess::CurrentLayout(shell).sidebar;
  const float x = std::min(row->x + 40.0f, sidebar.x + sidebar.w - 10.0f);
  const float y = row->y + row->h / 2.0f;
  Expect(x > row->x && x < sidebar.x + sidebar.w, "the probe point should be over the menu and the tree");
  const auto hover = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight, DispatchEvents(shell, {MouseMotionEvent(x, y)}), pin_caret);
  Expect(hover.identical(), "hovering a menu row over the tree should paint what a full repaint paints; " +
                                DescribePixelDiff(hover));
}

void TestWorkspaceShellMenuRowOverTheSidebarViewButtonsRunsTheRow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the menu click fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  Expect(WorkspaceShellTestAccess::SetSettingValue(shell, "chrome.project_tabs.hide_when_single", "false"),
         "the fixture should show the project strip");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  WorkspaceShellTestAccess::OpenFile(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("// edited\n");
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const std::optional<SDL_FRect> file = WorkspaceShellTestAccess::MenuBarItemRect(shell, "File");
  Expect(file.has_value(), "the File menu should be on the menu bar");
  (void)DispatchEvents(shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, file->x + 4.0f, file->y + 4.0f),
                               MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, file->x + 4.0f, file->y + 4.0f)});
  Expect(WorkspaceShellTestAccess::MenuBarOpen(shell), "clicking File should open it");

  const std::string view_before = WorkspaceShellTestAccess::SidebarViewId(shell);
  std::vector<std::string> views{"tree", "search", "git"};
  std::stable_partition(views.begin(), views.end(), [&](const std::string& v) { return v != view_before; });
  std::optional<SDL_FPoint> probe;
  std::string probed_label;
  for (const std::string& label : WorkspaceShellTestAccess::VisiblePopupMenuLabels(shell, WorkspaceShell::MenuId::File)) {
    const std::optional<SDL_FRect> row =
        WorkspaceShellTestAccess::PopupMenuItemRect(shell, WorkspaceShell::MenuId::File, label);
    for (const std::string& view : views) {
      SDL_FRect overlap{};
      const SDL_FRect button = WorkspaceShellTestAccess::SidebarModeTabRect(shell, view);
      if (!probe.has_value() && row.has_value() && label != "Quit" && label != "Close Project" &&
          SDL_GetRectIntersectionFloat(&*row, &button, &overlap) && overlap.w > 2.0f && overlap.h > 2.0f) {
        probe = SDL_FPoint{overlap.x + overlap.w / 2.0f, overlap.y + overlap.h / 2.0f};
        probed_label = label;
      }
    }
  }
  Expect(probe.has_value(), "the fixture should put a File row over a sidebar view button: " + [&] {
    std::string all;
    for (const std::string& label : WorkspaceShellTestAccess::VisiblePopupMenuLabels(shell, WorkspaceShell::MenuId::File)) {
      const auto row = WorkspaceShellTestAccess::PopupMenuItemRect(shell, WorkspaceShell::MenuId::File, label);
      all += label + "@" + (row ? std::to_string(row->y) : std::string("-")) + " ";
    }
    for (const std::string& view : views) {
      all += view + "@" + std::to_string(WorkspaceShellTestAccess::SidebarModeTabRect(shell, view).y) + " ";
    }
    return all;
  }());
  if (!probe.has_value()) {
    return;
  }

  (void)DispatchEvents(shell, {MouseMotionEvent(probe->x, probe->y),
                               MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, probe->x, probe->y),
                               MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, probe->x, probe->y)});
  Expect(WorkspaceShellTestAccess::SidebarViewId(shell) == view_before,
         "a click on a menu row must not switch the sidebar view beneath it");
  Expect(!WorkspaceShellTestAccess::MenuBarOpen(shell), "running a row should close the menu");
  const util::JsonValue ui = WorkspaceShellTestAccess::ExecuteControlQuery(shell, "ui");
  Expect(ui["menu"]["lastRun"].AsString() == "File/" + probed_label,
         "the ui receipt should name the row the click ran, not " + ui["menu"]["lastRun"].AsString());
  Expect(ui["menu"]["rowsRun"].AsInt() == 1, "one click should run one row");
}

void TestWorkspaceShellMenuTakingTheKeyboardRepaintsTheFieldFocusRing() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the focus ring fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  SoftwareCanvas canvas(kWidth, kHeight);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "");
  Expect(WorkspaceShellTestAccess::CurrentTextInputSurface(shell) ==
             microide::workspace::TextInputSurface::SidebarSearchQuery,
         "the search view's query field should own the keyboard");
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const std::optional<SDL_FRect> help = WorkspaceShellTestAccess::MenuBarItemRect(shell, "Help");
  Expect(help.has_value(), "the Help menu should be on the menu bar");
  const SDL_FRect sidebar = WorkspaceShellTestAccess::CurrentLayout(shell).sidebar;
  Expect(help->x > sidebar.x + sidebar.w, "the Help menu should drop clear of the sidebar");
  const float x = help->x + help->w / 2.0f;
  const float y = help->y + help->h / 2.0f;
  const auto open = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight,
      DispatchEvents(shell, {MouseMotionEvent(x, y),
                             MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                             MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)}),
      pin_caret);
  Expect(WorkspaceShellTestAccess::MenuBarOpen(shell), "the click should open the Help menu");
  Expect(open.identical(), "opening a menu should repaint the field it took the keyboard from; " +
                               DescribePixelDiff(open));
  const auto close = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight, DispatchEvents(shell, {KeyDownEvent(SDLK_ESCAPE)}), pin_caret);
  Expect(!WorkspaceShellTestAccess::MenuBarOpen(shell), "Escape should close the Help menu");
  Expect(close.identical(), "closing the menu should repaint the field it gives the keyboard back to; " +
                                DescribePixelDiff(close));
}

void TestWorkspaceShellOpeningAMenuTakesTheCaretOffScreen() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::string body;
  for (int line = 0; line < 40; ++line) {
    body += "int value_" + std::to_string(line) + " = " + std::to_string(line) + ";\n";
  }
  WriteFile(root / "alpha.cpp", body);

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the caret fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)DispatchEvents(shell, {KeyDownEvent(SDLK_END, SDL_KMOD_CTRL)});
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  Expect(WorkspaceShellTestAccess::CaretVisibleNow(shell), "the editor caret should be showing");

  const std::optional<SDL_FRect> file = WorkspaceShellTestAccess::MenuBarItemRect(shell, WorkspaceShell::MenuId::File);
  Expect(file.has_value(), "the File menu should be on the menu bar");
  const float x = file->x + file->w / 2.0f;
  const float y = file->y + file->h / 2.0f;
  const auto open = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight,
      DispatchEvents(shell, {MouseMotionEvent(x, y),
                             MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                             MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)}),
      pin_caret);
  Expect(WorkspaceShellTestAccess::MenuBarOpen(shell), "the click should open the File menu");
  Expect(open.identical(), "opening a menu should take the caret off screen in that frame; " +
                               DescribePixelDiff(open));
}

void TestWorkspaceShellEditRepaintsEveryPaneOfTheDocument() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\nint other = 2;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the split fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, microide::workspace::EditorSplitOrientation::Vertical) ||
             WorkspaceShellTestAccess::EditorGroupCount(shell) == 2,
         "the editor should split");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const auto typed = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight, DispatchEvents(shell, {TextInputEvent("xyz")}), pin_caret);
  Expect(typed.identical(), "typing in one pane should repaint the other pane of the document; " +
                                DescribePixelDiff(typed));
}

void TestWorkspaceShellTerminalIsFixedForAPreparedFrame() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the terminal fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 8, 80);
  TerminalSessionTestAccess::AppendOutput(session, "prompt$ ");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);

  SoftwareCanvas canvas(kWidth, kHeight);
  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks());
  shell.Render(canvas.renderer(), kWidth, kHeight);
  const WorkspaceShell::FrameToken token = shell.PrepareFrameOnce(canvas.renderer(), kWidth, kHeight);
  shell.RenderClip(token, canvas.renderer(), kWidth, kHeight);
  SDL_FlushRenderer(canvas.renderer());
  SDL_Surface* first = SDL_DuplicateSurface(canvas.surface());

  TerminalSessionTestAccess::AppendOutput(session, "echo hello");
  shell.RenderClip(token, canvas.renderer(), kWidth, kHeight);
  SDL_FlushRenderer(canvas.renderer());
  const auto diff = DiffSurfaces(first, canvas.surface());
  SDL_DestroySurface(first);
  Expect(diff.identical(), "two renders of one prepared frame should paint the same terminal; " +
                               DescribePixelDiff(diff));
}

void TestWorkspaceShellMaximizedPanelPaintsNoHiddenSurface() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the panel fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  TerminalSessionTestAccess::Reset(WorkspaceShellTestAccess::ActiveTerminalSession(shell), 8, 80);
  WorkspaceShellTestAccess::TogglePanelMaximizedForTest(shell);
  Expect(WorkspaceShellTestAccess::PanelMaximized(shell), "the panel should be maximized");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  WorkspaceShell::RenderInvalidation whole;
  whole.rects.push_back(SDL_FRect{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight)});
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, whole, pin_caret);
  Expect(diff.identical(), "a hinted and an unhinted render of a maximized panel should agree; " +
                               DescribePixelDiff(diff));
}

void TestWorkspaceShellAStatusSegmentModalOwnsThePointer() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the status fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const std::optional<SDL_FRect> indent =
      WorkspaceShellTestAccess::StatusBarSegmentRect(shell, microide::workspace::StatusBarSegmentId::Indent);
  const std::optional<SDL_FRect> encoding =
      WorkspaceShellTestAccess::StatusBarSegmentRect(shell, microide::workspace::StatusBarSegmentId::Encoding);
  Expect(indent.has_value() && encoding.has_value(), "the indent and encoding segments should show");
  const float x = indent->x + indent->w / 2.0f;
  const float y = indent->y + indent->h / 2.0f;
  const auto step = [&](std::string_view what, std::initializer_list<SDL_Event> events) {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                  DispatchEvents(shell, events), pin_caret);
    Expect(diff.identical(), std::string(what) + " should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  };
  const std::optional<SDL_FRect> project =
      WorkspaceShellTestAccess::StatusBarSegmentRect(shell, microide::workspace::StatusBarSegmentId::Project);
  if (project.has_value()) {
    const float px = project->x + project->w / 2.0f;
    const float py = project->y + project->h / 2.0f;
    step("clicking the project segment", {MouseMotionEvent(px, py),
                                          MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, px, py),
                                          MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, px, py)});
  }
  step("hovering the indent segment", {MouseMotionEvent(x, y)});
  step("clicking it", {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                       MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)});
  step("moving to the encoding segment",
       {MouseMotionEvent(encoding->x + encoding->w / 2.0f, encoding->y + encoding->h / 2.0f)});
}

void TestBreadcrumbFolderAtNamesTheSegmentUnderTheClick() {
  using microide::workspace::BreadcrumbFolderAt;
  const auto width = [](std::string_view text) { return static_cast<float>(text.size()) * 10.0f; };
  Expect(BreadcrumbFolderAt("src/ui/view.cpp", 100.0f, 115.0f, width) == std::optional<std::string>("src"),
         "a click on src names src");
  Expect(BreadcrumbFolderAt("src/ui/view.cpp", 100.0f, 150.0f, width) == std::optional<std::string>("src/ui"),
         "a click on ui names src/ui");
  Expect(BreadcrumbFolderAt("src/ui/view.cpp", 100.0f, 200.0f, width) == std::optional<std::string>("src/ui"),
         "a click on the file names its folder");
  Expect(BreadcrumbFolderAt("main.cpp", 100.0f, 120.0f, width) == std::optional<std::string>(""),
         "a file at the top names the project's own folder");
  Expect(!BreadcrumbFolderAt("src/ui/view.cpp", 100.0f, 90.0f, width).has_value() &&
             !BreadcrumbFolderAt("src/ui/view.cpp", 100.0f, 400.0f, width).has_value(),
         "off the text names nothing");
}

void TestWorkspaceShellChromeSegmentsAreInteractive() {
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "src" / "ui" / "view.cpp", "int v = 1;\n");
  WriteFile(root / "src" / "ui" / "other.cpp", "int o = 2;\n");
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the project opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "src" / "ui" / "view.cpp");
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  const auto click = [&](const SDL_FRect& rect) {
    const float x = rect.x + rect.w / 2.0f;
    const float y = rect.y + rect.h / 2.0f;
    (void)SendMouseDown(shell, x, y, SDL_BUTTON_LEFT);
    (void)SendMouseUp(shell, x, y, SDL_BUTTON_LEFT);
  };
  using microide::workspace::StatusBarSegmentId;

  const auto eol = WorkspaceShellTestAccess::StatusBarSegmentRect(shell, StatusBarSegmentId::LineEnding);
  Expect(eol.has_value(), "the line ending has a segment of its own");
  click(*eol);
  const auto& pick = WorkspaceShellTestAccess::QuickPick(shell);
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) && pick.title == "Change End of Line Sequence" &&
             pick.items.size() == 2,
         "clicking it offers LF and CRLF");
  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE) && SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "Down, Enter chooses CRLF");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).line_ending() == microide::util::LineEnding::CRLF &&
             WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "the file is CRLF now, and dirty");
  Expect(SendKeyDown(shell, SDLK_Z, SDL_KMOD_CTRL), "Ctrl+Z undoes it");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).line_ending() == microide::util::LineEnding::LF &&
             !WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "back to LF and clean");

  shell.Render(canvas.renderer(), 1280, 720);
  const auto indent = WorkspaceShellTestAccess::StatusBarSegmentRect(shell, StatusBarSegmentId::Indent);
  Expect(indent.has_value(), "the indentation segment shows");
  click(*indent);
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) &&
             WorkspaceShellTestAccess::QuickPick(shell).title == "Indentation",
         "clicking it offers the indentation choices, not Settings");
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE), "Escape closes it");

  shell.Render(canvas.renderer(), 1280, 720);
  const auto rects = WorkspaceShellTestAccess::EditorGroupRectsFor(shell, WorkspaceShellTestAccess::CurrentLayout(shell));
  const SDL_FRect band = rects.groups[0].breadcrumb;
  (void)SendMouseDown(shell, band.x + 14.0f, band.y + band.h / 2.0f, SDL_BUTTON_LEFT);
  const auto& browse = WorkspaceShellTestAccess::QuickPick(shell);
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) && browse.title == "src" &&
             std::any_of(browse.items.begin(), browse.items.end(),
                         [](const auto& item) { return item.primary_label == "ui/"; }),
         "a click on the src segment browses src: " + browse.title);
}

void TestWorkspaceShellSettingsOverAFocusedTerminalHidesItsCaret() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the terminal fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 8, 80);
  TerminalSessionTestAccess::AppendOutput(session, "prompt$ ");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  Expect(WorkspaceShellTestAccess::CaretVisibleNow(shell), "the terminal caret should be showing");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "settings"), "the settings command runs");
  const auto open = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight, shell.ConsumePendingRenderInvalidation(), pin_caret);
  Expect(WorkspaceShellTestAccess::SettingsOverlayVisible(shell), "Settings opens over the terminal");
  Expect(open.identical(), "opening Settings over a focused terminal should paint what a full "
                           "repaint paints; " + DescribePixelDiff(open));
  Expect(!WorkspaceShellTestAccess::ShouldBlinkCaret(shell),
         "no caret should blink under the Settings overlay");
  const auto typed = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight, DispatchEvents(shell, {TextInputEvent("x")}), pin_caret);
  Expect(typed.identical(), "typing in Settings over a terminal should paint what a full "
                            "repaint paints; " + DescribePixelDiff(typed));
}

void TestWorkspaceShellClosingAnAnchoredMenuReleasesItsButton() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the sidebar fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const SDL_FRect more = WorkspaceShellTestAccess::SidebarModeOverflowRect(shell);
  Expect(more.w > 0.0f, "the sidebar should show its more-views button");
  const float x = more.x + more.w / 2.0f;
  const float y = more.y + more.h / 2.0f;
  const auto open = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight,
      DispatchEvents(shell, {MouseMotionEvent(x, y), MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                             MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)}),
      pin_caret);
  Expect(WorkspaceShellTestAccess::MenuBarOpen(shell), "the click should open the views menu");
  Expect(open.identical(), "opening the views menu should paint what a full repaint paints; " +
                               DescribePixelDiff(open));
  const auto away = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight, DispatchEvents(shell, {MouseMotionEvent(700.0f, 400.0f)}),
      pin_caret);
  Expect(away.identical(), "moving off the menu should paint what a full repaint paints; " +
                               DescribePixelDiff(away));
  const auto closed = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight,
      DispatchEvents(shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, 700.0f, 400.0f),
                             MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, 700.0f, 400.0f)}),
      pin_caret);
  Expect(!WorkspaceShellTestAccess::MenuBarOpen(shell), "a click outside should close the views menu");
  Expect(closed.identical(), "closing the views menu should release its button; " +
                                 DescribePixelDiff(closed));
}

void TestWorkspaceShellAModalReleasesAHoveredWindowControl() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the window-control fixture should open its project");
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const std::optional<SDL_FRect> maximize =
      WorkspaceShellTestAccess::WindowControlButtonRect(shell, WorkspaceShell::WindowControlButtonId::Maximize);
  Expect(maximize.has_value(), "the window should show its maximize button");
  const auto hover = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight,
      DispatchEvents(shell, {MouseMotionEvent(maximize->x + maximize->w / 2.0f,
                                              maximize->y + maximize->h / 2.0f)}),
      pin_caret);
  Expect(hover.identical(), "hovering the maximize button should paint what a full repaint "
                            "paints; " + DescribePixelDiff(hover));
  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);
  const auto modal = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                 shell.ConsumePendingRenderInvalidation(), pin_caret);
  Expect(modal.identical(), "a modal opening should take the hover off the window control; " +
                                DescribePixelDiff(modal));
}

void TestWorkspaceShellMenuBarFitsOrOverflowsNeverTruncates() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the fixture should open its project");
  std::vector<WorkspaceShell::MenuId> bar_menus;
  for (const auto& spec : microide::workspace::WorkspaceMenuSpecs()) {
    if (IsMenuBarTopLevelMenu(spec.id)) {
      bar_menus.push_back(spec.id);
    }
  }
  int fits_from = -1;
  float needed = 0.0f;
  for (int width = 720; width <= 1600; width += 4) {
    WorkspaceShellTestAccess::SetWindowSize(shell, width, 720);
    WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
    const SDL_FRect bar = WorkspaceShellTestAccess::MenuBarRect(shell);
    const auto on_bar = WorkspaceShellTestAccess::MenuBarItemsForTest(shell);
    const auto overflow = WorkspaceShellTestAccess::MenuBarOverflowIdsForTest(shell);
    const auto chevron = WorkspaceShellTestAccess::MenuOverflowChevronRect(shell);
    std::vector<WorkspaceShell::MenuId> order;
    float right = bar.x;
    for (const auto& item : on_bar) {
      order.push_back(item.id);
      Expect(item.rect.x >= right && item.rect.w > 0.0f,
             "bar menus should be laid out left to right without overlap at width " + std::to_string(width));
      right = item.rect.x + item.rect.w;
    }
    order.insert(order.end(), overflow.begin(), overflow.end());
    Expect(order == bar_menus, "every bar menu should be on the bar or in the chevron, in order, at width " +
                                   std::to_string(width));
    Expect(overflow.empty() == !chevron.has_value(),
           "the chevron should show exactly when menus overflow, at width " + std::to_string(width));
    if (chevron.has_value()) {
      Expect(right <= chevron->x, "no bar menu should run under the chevron at width " + std::to_string(width));
    }
    if (fits_from < 0 && overflow.empty()) {
      fits_from = width;
      needed = right - bar.x;
    }
  }
  constexpr int kMenusExtentPx = 628;
  Expect(fits_from == 720, "every bar menu should fit at the narrowest Normal width, fits from " +
                               std::to_string(fits_from));
  Expect(static_cast<int>(needed + 0.5f) == kMenusExtentPx,
         "the bar menus need " + std::to_string(needed) + "px, pinned at " + std::to_string(kMenusExtentPx));
}

void TestWorkspaceShellMenuArrowsWalkOnlyTheMenuBar() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the menu fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);

  const std::optional<SDL_FRect> help = WorkspaceShellTestAccess::MenuBarItemRect(shell, WorkspaceShell::MenuId::Help);
  Expect(help.has_value(), "Help should be on the menu bar");
  (void)DispatchEvents(shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, help->x + 4.0f, help->y + 4.0f),
                               MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, help->x + 4.0f, help->y + 4.0f)});
  Expect(WorkspaceShellTestAccess::ActiveMenuId(shell) == WorkspaceShell::MenuId::Help, "Help should open");
  (void)DispatchEvents(shell, {KeyDownEvent(SDLK_RIGHT)});
  Expect(WorkspaceShellTestAccess::ActiveMenuId(shell) == WorkspaceShell::MenuId::File,
         "Right from the last bar menu should wrap to File");
  (void)DispatchEvents(shell, {KeyDownEvent(SDLK_LEFT)});
  Expect(WorkspaceShellTestAccess::ActiveMenuId(shell) == WorkspaceShell::MenuId::Help,
         "Left from File should wrap to the last bar menu");
  (void)DispatchEvents(shell, {KeyDownEvent(SDLK_ESCAPE)});

  const SDL_FRect more = WorkspaceShellTestAccess::SidebarModeOverflowRect(shell);
  Expect(more.w > 0.0f, "the sidebar should show its more-views button");
  (void)DispatchEvents(shell, {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, more.x + 4.0f, more.y + 4.0f),
                               MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, more.x + 4.0f, more.y + 4.0f)});
  Expect(WorkspaceShellTestAccess::ActiveMenuId(shell) == WorkspaceShell::MenuId::SidebarMode,
         "the more-views button should open the views menu");
  (void)DispatchEvents(shell, {KeyDownEvent(SDLK_RIGHT)});
  Expect(WorkspaceShellTestAccess::ActiveMenuId(shell) == WorkspaceShell::MenuId::SidebarMode,
         "Right in an anchored menu should stay in it");
}

void TestWorkspaceShellPanelHeaderButtonRepaintsItsHover() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the panel fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  TerminalSessionTestAccess::Reset(WorkspaceShellTestAccess::ActiveTerminalSession(shell), 8, 80);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const SDL_FRect maximize = WorkspaceShellTestAccess::BottomPanelMaximizeButtonRect(shell);
  Expect(maximize.w > 0.0f, "the panel should show its maximize button");
  for (const SDL_FRect& target : {maximize, SDL_FRect{maximize.x - 200.0f, maximize.y, 1.0f, 1.0f}}) {
    const auto diff = PaintAndDiffWithFullRepaint(
        shell, canvas, kWidth, kHeight,
        DispatchEvents(shell, {MouseMotionEvent(target.x + target.w / 2.0f, target.y + target.h / 2.0f)}),
        pin_caret);
    Expect(diff.identical(), "moving onto and off the maximize button should paint what a full "
                             "repaint paints; " + DescribePixelDiff(diff));
  }
}

void TestWorkspaceShellRemoteProjectTabShowsHostAndConnectionCorner() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path local_root = temp_dir.path() / "local";
  const std::filesystem::path remote_root = temp_dir.path() / "app";
  WriteFile(local_root / "a.txt", "a\n");
  WriteFile(remote_root / "b.txt", "b\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, local_root, false, false),
         "the fixture should open the local project");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, remote_root, false, false),
         "the fixture should open the second project");
  auto& remote = WorkspaceShellTestAccess::CurrentProjectState(shell);
  remote.id = microide::project::ProjectId::ForRemote("worky", "/srv/app");
  using Stage = microide::editor::WelcomeStageState;
  remote.remote_status.stages = {{.text = "not connected", .state = Stage::Failed}};
  ++remote.remote_status_revision;

  const auto local_tab = WorkspaceShellTestAccess::VisibleProjectTab(shell, 0);
  const auto remote_tab = WorkspaceShellTestAccess::VisibleProjectTab(shell, 1);
  Expect(local_tab.has_value() && remote_tab.has_value(), "both project tabs should be on the strip");
  Expect(local_tab->secondary_title.empty() && !local_tab->remote_state.has_value(),
         "a local project's tab has no host and no corner");
  Expect(remote_tab->secondary_title == " \xc2\xb7 worky",
         "a remote project's tab names its host: '" + remote_tab->secondary_title + "'");
  Expect(remote_tab->remote_state == Stage::Failed, "the corner carries the connection's worst stage");

  const auto theme = microide::render::MakeDefaultTheme();
  const auto same = [](SDL_Color a, SDL_Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
  SoftwareCanvas canvas(kWidth, kHeight);
  const auto pixel_of = [&](std::size_t index, int row) {
    shell.Render(canvas.renderer(), kWidth, kHeight);
    const SDL_FRect rect = WorkspaceShellTestAccess::ProjectTabRect(shell, index);
    return canvas.PixelAt(static_cast<int>(rect.x) + 1, static_cast<int>(rect.y) + row);
  };
  const auto corner_of = [&](std::size_t index) { return pixel_of(index, 3); };
  Expect(same(corner_of(1), theme.diagnostic_error), "an offline project's corner is the error colour");
  Expect(!same(pixel_of(1, 0), theme.diagnostic_error) && !same(pixel_of(1, 1), theme.diagnostic_error),
         "the active tab's accent is not painted over by the corner");
  Expect(same(pixel_of(1, 2), theme.diagnostic_error),
         "the corner starts right under the active tab's accent, with no gap");
  Expect(!same(corner_of(0), theme.diagnostic_error) && !same(corner_of(0), theme.diff_added),
         "a local project's corner is the tab's own");

  remote.remote_status.stages = {{.text = "connected", .state = Stage::Done},
                                 {.text = "synced 2 files", .state = Stage::Done}};
  ++remote.remote_status_revision;
  Expect(same(corner_of(1), theme.diff_added),
         "once connected and synced, the same tab's corner turns to the done colour");

  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0), "switching to the local project");
  Expect(same(pixel_of(1, 0), theme.diff_added) && same(pixel_of(1, 1), theme.diff_added),
         "an inactive tab's corner starts at its top edge, with no gap above it");
}

void TestWorkspaceShellEmptyTerminalPanelShowsItsNewTerminalButton() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the panel fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::CurrentProjectState(shell).terminal_tabs.clear();
  WorkspaceShellTestAccess::CurrentProjectState(shell).active_terminal_tab_index = 0;
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);

  SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);

  const SDL_FRect button = WorkspaceShellTestAccess::BottomPanelTerminalNewTabRect(shell);
  Expect(button.w > 0.0f, "the empty Terminal panel should lay out its New Terminal button");
  const int cx = static_cast<int>(std::floor(button.x + button.w * 0.5f));
  const int cy = static_cast<int>(std::floor(button.y + button.h * 0.5f));
  const SDL_Color glyph = canvas.PixelAt(cx, cy);
  const SDL_Color beside = canvas.PixelAt(cx + 2, cy + 2);
  Expect(glyph.r != beside.r || glyph.g != beside.g || glyph.b != beside.b,
         "the empty Terminal panel should paint the + of its New Terminal button");
}

void TestWorkspaceShellDroppingAPanelTabClearsItsGhost() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");
  WriteFile(root / "beta.cpp", "int beta = 2;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the panel fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::AddTerminalTab(shell);
  TerminalSessionTestAccess::Reset(WorkspaceShellTestAccess::ActiveTerminalSession(shell), 8, 80);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const SDL_FRect tab = WorkspaceShellTestAccess::TerminalTabRect(shell, 1);
  Expect(tab.w > 0.0f, "the panel should show the terminal tab");
  const float x = tab.x + tab.w / 3.0f;
  const float y = tab.y + tab.h / 2.0f;
  SDL_Event held = MouseMotionEvent(x, y);
  const auto step = [&](std::string_view what, std::initializer_list<SDL_Event> events) {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                  DispatchEvents(shell, events), pin_caret);
    Expect(diff.identical(), std::string(what) + " should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  };
  step("pressing the tab", {MouseMotionEvent(x, y), MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y)});
  for (float dx : {12.0f, 30.0f, 58.0f}) {
    held = MouseMotionEvent(x + dx, y);
    held.motion.state = SDL_BUTTON_LMASK;
    step("dragging the tab", {held});
  }
  step("dropping the tab", {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x + 58.0f, y)});

  WorkspaceShellTestAccess::OpenFileInNewTab(shell, root / "beta.cpp");
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const SDL_FRect editor_tab = WorkspaceShellTestAccess::EditorTabRect(shell, 1);
  const float ex = editor_tab.x + editor_tab.w / 3.0f;
  const float ey = editor_tab.y + editor_tab.h / 2.0f;
  step("pressing an editor tab", {MouseMotionEvent(ex, ey), MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, ex, ey)});
  for (float dx : {-12.0f, -40.0f}) {
    held = MouseMotionEvent(ex + dx, ey);
    held.motion.state = SDL_BUTTON_LMASK;
    step("dragging the editor tab", {held});
  }
  step("dropping the editor tab", {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, ex - 40.0f, ey)});
}

void TestWorkspaceShellClosingSettingsOverTheFindWidgetRepaintsItsArea() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the settings fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const auto step = [&](std::string_view what, const microide::workspace::RenderInvalidation& redraw) {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, redraw, pin_caret);
    Expect(diff.identical(), std::string(what) + " should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  };
  step("opening the find widget", DispatchEvents(shell, {KeyDownEvent(SDLK_F, SDL_KMOD_CTRL)}));
  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);
  step("opening Settings", shell.ConsumePendingRenderInvalidation());
  step("closing Settings", DispatchEvents(shell, {KeyDownEvent(SDLK_ESCAPE)}));
}

void TestWorkspaceShellSidebarDividerDragPaintsCleanly() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the divider fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const SDL_FRect handle = microide::workspace::SidebarResizeHitRect(WorkspaceShellTestAccess::CurrentLayout(shell));
  const float x = handle.x + handle.w / 2.0f;
  const float y = handle.y + handle.h / 2.0f;
  const auto step = [&](std::string_view what, std::initializer_list<SDL_Event> events) {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                  DispatchEvents(shell, events), pin_caret);
    Expect(diff.identical(), std::string(what) + " should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  };
  step("hovering the divider", {MouseMotionEvent(x, y)});
  step("pressing the divider", {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y)});
  for (float dx : {-20.0f, -60.0f, -98.0f}) {
    SDL_Event held = MouseMotionEvent(x + dx, y + 30.0f);
    held.motion.state = SDL_BUTTON_LMASK;
    step("dragging the divider", {held});
  }
  step("releasing the divider", {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x - 98.0f, y + 30.0f)});
  step("moving away", {MouseMotionEvent(x + 300.0f, y)});
}

void TestWorkspaceShellPanelDividerResetRepaintsTheLayout() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the panel fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  TerminalSessionTestAccess::Reset(WorkspaceShellTestAccess::ActiveTerminalSession(shell), 8, 80);
  WorkspaceShellTestAccess::SetBottomPanelHeight(shell, 300.0f);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const SDL_FRect handle = microide::workspace::BottomPanelResizeHitRect(WorkspaceShellTestAccess::CurrentLayout(shell));
  const float x = handle.x + handle.w / 2.0f;
  const float y = handle.y + handle.h / 2.0f;
  SDL_Event press = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
  press.button.clicks = 2;
  SDL_Event release = MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
  release.button.clicks = 2;
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                DispatchEvents(shell, {press, release}), pin_caret);
  Expect(diff.identical(), "resetting the panel height should paint what a full repaint paints; " +
                               DescribePixelDiff(diff));
}

void TestWorkspaceShellAStackedModalRedimsTheWindow() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the modal fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  WorkspaceShellTestAccess::OpenCommandPalette(shell);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "goto"), "goto should run");
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell), "goto should open its prompt");
  microide::workspace::RenderInvalidation dialog_only;
  const SDL_FRect dialog = microide::workspace::ComputePromptSurfaceRect(
      WorkspaceShellTestAccess::CurrentLayout(shell).full);
  dialog_only.rects.push_back(dialog);
  (void)shell.ConsumePendingRenderInvalidation();
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, dialog_only, pin_caret);
  Expect(diff.identical(), "a prompt over the palette should re-dim the whole window; " +
                               DescribePixelDiff(diff));
}

void TestWorkspaceShellTabTooltipFollowsATabMovedUnderThePointer() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");
  WriteFile(root / "beta.cpp", "int beta = 2;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the tab fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::OpenFileInNewTab(shell, root / "beta.cpp");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const SDL_FRect first = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  const auto hover = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight,
      DispatchEvents(shell, {MouseMotionEvent(first.x + first.w / 2.0f, first.y + first.h / 2.0f)}),
      pin_caret);
  Expect(hover.identical(), "hovering a tab should paint what a full repaint paints; " +
                                DescribePixelDiff(hover));
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tabmove -1"), "tabmove should run");
  const auto moved = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                 shell.ConsumePendingRenderInvalidation(), pin_caret);
  Expect(moved.identical(), "a tab moved under the pointer should repaint its tooltip; " +
                                DescribePixelDiff(moved));
}

void TestWorkspaceShellNarrowGitViewKeepsItsLabelsInside() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add narrow git fixture", "narrow git fixture");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the git fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::SetSettingValue(shell, "editor.blame.inline.enabled", "false");
  WorkspaceShellTestAccess::ShowGitSidebar(shell);
  Expect(SettleGitSidebarRefresh(shell), "the git view should settle");
  WorkspaceShellTestAccess::SetSidebarWidth(shell, 190.0f);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  microide::workspace::RenderInvalidation editor_only;
  editor_only.rects.push_back(WorkspaceShellTestAccess::CurrentLayout(shell).editor_surface);
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, editor_only, pin_caret);
  Expect(diff.identical(), "a narrow git view should paint inside itself; " + DescribePixelDiff(diff));
}

void TestWorkspaceShellTreeFollowingTheActiveTabRepaints() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");
  WriteFile(root / "beta.cpp", "int beta = 2;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the tree fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const std::size_t before = WorkspaceShellTestAccess::TreeSelectedIndex(shell);
  WorkspaceShellTestAccess::SetTreeSelectedIndexSilently(shell, before == 0 ? 1 : 0);
  microide::workspace::RenderInvalidation elsewhere;
  elsewhere.rects.push_back(WorkspaceShellTestAccess::CurrentLayout(shell).status_bar);
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, elsewhere, pin_caret);
  Expect(diff.identical(), "a tree selection moved with no damage should repaint on the next frame; " +
                               DescribePixelDiff(diff));
}

void TestWorkspaceShellEditorSplitDividerDragPaintsCleanly() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the split-divider fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Vertical),
         "the fixture should split the editor");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const auto dividers = WorkspaceShellTestAccess::EditorSplitDividerRects(shell);
  Expect(dividers.size() == 1, "the split should have one divider");
  const SDL_FRect handle = dividers[0].rect;
  const float x = handle.x + handle.w / 2.0f;
  const float y = handle.y + handle.h / 2.0f;
  const auto step = [&](std::string_view what, std::initializer_list<SDL_Event> events) {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                  DispatchEvents(shell, events), pin_caret);
    Expect(diff.identical(), std::string(what) + " should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  };
  step("hovering the split divider", {MouseMotionEvent(x, y)});
  step("pressing the split divider", {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y)});
  for (float dx : {20.0f, 60.0f}) {
    SDL_Event held = MouseMotionEvent(x + dx, y);
    held.motion.state = SDL_BUTTON_LMASK;
    step("dragging the split divider", {held});
  }
  step("releasing the split divider", {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x + 60.0f, y)});
  step("moving away", {MouseMotionEvent(x - 300.0f, y)});
}

void TestWorkspaceShellPaneTabStripRepaintKeepsTheSplitDivider() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the split fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Vertical),
         "the fixture should split the editor");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const SDL_FRect tab = WorkspaceShellTestAccess::GroupEditorTabRect(shell, 1, 0);
  Expect(tab.w > 0.0f, "the right pane should show its tab");
  const auto step = [&](std::string_view what, std::initializer_list<SDL_Event> events) {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                  DispatchEvents(shell, events), pin_caret);
    Expect(diff.identical(), std::string(what) + " should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  };
  step("hovering the right pane's first tab",
       {MouseMotionEvent(tab.x + 4.0f, tab.y + tab.h / 2.0f)});
  step("leaving it", {MouseMotionEvent(tab.x + 4.0f, tab.y + tab.h + 200.0f)});
}

void TestWorkspaceShellSaveAsRepaintsTheTabTitle() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "blocker.txt", "a file, so nothing can be created under it\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the save-as fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tab"), "an untitled tab opens");
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("hello");
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "save notes/named.txt"),
         "save <path> names the buffer");
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                shell.ConsumePendingRenderInvalidation(), pin_caret);
  Expect(diff.identical(),
         "the renamed tab should paint what a full repaint paints; " + DescribePixelDiff(diff));

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tab"), "a second untitled tab");
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("again");
  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "save blocker.txt/inner.txt"),
         "a Save As that cannot write fails");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path().empty(),
         "and leaves the buffer untitled, not named after a file that does not exist");
}

void TestWorkspaceShellAPromptOverSettingsOwnsThePointer() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const SDL_FRect row = WorkspaceShellTestAccess::SettingsOverlayVisibleRowRect(shell, 1);
  Expect(row.w > 0.0f, "Settings should show a second row");
  const auto step = [&](std::string_view what, std::initializer_list<SDL_Event> events) {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                  DispatchEvents(shell, events), pin_caret);
    Expect(diff.identical(), std::string(what) + " should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  };
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::Goto, {}),
         "Go to Line should run from the menu");
  {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                  shell.ConsumePendingRenderInvalidation(), pin_caret);
    Expect(diff.identical(), "opening Go to Line over Settings should paint what a full repaint "
                             "paints; " + DescribePixelDiff(diff));
  }
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell) &&
             WorkspaceShellTestAccess::SettingsOverlayVisible(shell),
         "Go to Line should open over Settings");
  const float x = row.x + 20.0f;
  const float y = row.y + row.h / 2.0f;
  SDL_Surface* before = SDL_DuplicateSurface(canvas.surface());
  step("moving onto a Settings row outside the prompt", {MouseMotionEvent(x, y)});
  step("wheeling there", {MouseWheelEvent(x, y, -2.0f)});
  Expect(CountPixelDifferences(before, canvas.surface()) == 0,
         "a Settings row under a prompt must not hover or scroll");
  SDL_DestroySurface(before);
  step("clicking there", {MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                          MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)});
}

void TestWorkspaceShellSettingsFilterTextStaysInItsBar() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);
  const std::string long_query(400, 'x');
  (void)shell.HandleEvent(TextInputEvent(long_query.c_str()));

  const auto [bar, text_width] = WorkspaceShellTestAccess::SettingsOverlayFilterRectAndTextWidth(shell);
  Expect(bar.w > 0.0f, "the filter bar should be laid out");
  Expect(text_width > bar.w / 2.0f, "the query should fill the bar");
  Expect(text_width <= bar.w - 16.0f + 0.5f,
         "the shown query must fit inside the bar's insets, not run past it");
}

void TestWorkspaceShellReplacingTheFindWidgetRepaintsItsMatches() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "notes.md", "}b\nalpha alpha\n");
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "notes.md");
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const auto step = [&](std::string_view what, const microide::workspace::RenderInvalidation& redraw) {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, redraw, pin_caret);
    Expect(diff.identical(), std::string(what) + " should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  };
  step("opening find", DispatchEvents(shell, {KeyDownEvent(SDLK_F, SDL_KMOD_CTRL)}));
  step("searching alpha", DispatchEvents(shell, {TextInputEvent("alpha")}));
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "completion"),
         "a completion request opens its overlay");
  step("the completion list replacing find", shell.ConsumePendingRenderInvalidation());
}

void TestWorkspaceShellAnEditorRepaintKeepsTheDebugPaneDivider() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int alpha = 1;\n");
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "debug.enabled", "true"),
         "the debug pane needs the debugger on");
  WorkspaceShellTestAccess::SetDebugPaneVisible(shell, true);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  Expect(layout.right_pane.w > 0.0f, "the debug pane should be laid out");
  const float divider_x = layout.right_pane.x - 1.0f;
  const float right_padding = shell.PartialRedrawClipPadding().right;
  microide::workspace::RenderInvalidation redraw;
  redraw.rects.push_back(microide::workspace::MakeRect(
      layout.editor_area.x + 40.0f, layout.editor_area.y + 40.0f,
      divider_x + 1.0f - right_padding - (layout.editor_area.x + 40.0f), 60.0f));
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, redraw, pin_caret);
  Expect(diff.identical(),
         "an editor-width repaint should paint what a full repaint paints; " + DescribePixelDiff(diff));
}

void TestWorkspaceShellASaveRepaintsTheDirtyMarker() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "notes.md", "# notes\n");
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "notes.md");
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const auto step = [&](std::string_view what, const microide::workspace::RenderInvalidation& redraw) {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, redraw, pin_caret);
    Expect(diff.identical(), std::string(what) + " should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  };
  step("typing", DispatchEvents(shell, {TextInputEvent("x")}));
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "save"), "save from the command line");
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).dirty(), "the save wrote at once");
  step("saving", shell.ConsumePendingRenderInvalidation());
}

void TestWorkspaceShellSidebarShowRepaintsTheMovedEditor() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "notes.md", "# notes\nsome text\n");
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "notes.md");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "sidebar-hide"), "hide the sidebar");
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "sidebar-show"), "show it again");
  Expect(WorkspaceShellTestAccess::CurrentLayout(shell).sidebar.w > 0.0f,
         "the sidebar is laid out again");
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                shell.ConsumePendingRenderInvalidation(), pin_caret);
  Expect(diff.identical(),
         "showing the sidebar should paint what a full repaint paints; " + DescribePixelDiff(diff));
}

void TestWorkspaceShellASaveRepaintsTheProjectTabsDirtyMarker() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path other = temp_dir.path() / "other";
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(other / "a.txt", "a\n");
  WriteFile(root / "notes.md", "# notes\n");
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, other, false, false) &&
             WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "two projects, so the project strip shows");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "notes.md");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  Expect(WorkspaceShellTestAccess::CurrentLayout(shell).project_tab_strip.h > 0.0f,
         "the project strip is laid out");
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const auto step = [&](std::string_view what, const microide::workspace::RenderInvalidation& redraw) {
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, redraw, pin_caret);
    Expect(diff.identical(), std::string(what) + " should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  };
  step("typing", DispatchEvents(shell, {TextInputEvent("x")}));
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "save"), "save from the command line");
  step("saving", shell.ConsumePendingRenderInvalidation());
}

void TestWorkspaceShellATreeThatMovedRepaintsOnAnUnrelatedFrame() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  for (int i = 0; i < 60; ++i) {
    WriteFile(root / ("file_" + std::to_string(100 + i) + ".txt"), "x\n");
  }
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  WorkspaceShellTestAccess::SetSidebarScrollRow(shell, 20);
  microide::workspace::RenderInvalidation menu_bar_only;
  menu_bar_only.rects.push_back(WorkspaceShellTestAccess::CurrentLayout(shell).menu_bar);
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, menu_bar_only,
                                                pin_caret);
  Expect(diff.identical(), "a frame after the tree scrolled should paint what a full repaint "
                           "paints; " + DescribePixelDiff(diff));
}

void TestWorkspaceShellASplitDividerThatMovedRepaintsOnAnUnrelatedFrame() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Vertical),
         "the fixture should split the editor");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  Expect(WorkspaceShellTestAccess::ResizeRootSplitDivider(shell, 0, 0.6f), "move the divider");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  (void)shell.ConsumePendingRenderInvalidation();
  microide::workspace::RenderInvalidation menu_bar_only;
  menu_bar_only.rects.push_back(WorkspaceShellTestAccess::CurrentLayout(shell).menu_bar);
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, menu_bar_only,
                                                pin_caret);
  Expect(diff.identical(), "a frame after the divider moved should paint what a full repaint "
                           "paints; " + DescribePixelDiff(diff));
}

Uint64 HiddenCaretBlinkEpoch(WorkspaceShell& shell) {
  while (SDL_GetTicks() < 2000) {
    SDL_Delay(50);
  }
  for (Uint64 back = 50; back < 2000; back += 50) {
    WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks() - back);
    if (!WorkspaceShellTestAccess::CaretVisibleNow(shell)) {
      return SDL_GetTicks() - back;
    }
  }
  Expect(false, "the focused input's caret should blink");
  return 0;
}

void TestWorkspaceShellBlinkInALowerPaneKeepsTheDivider() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\nint other = 2;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the split fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, microide::workspace::EditorSplitOrientation::Horizontal),
         "the editor should split down");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);

  SoftwareCanvas canvas(kWidth, kHeight);
  const Uint64 hidden = HiddenCaretBlinkEpoch(shell);
  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, hidden);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  WorkspaceShell::RenderInvalidation invalidation;
  invalidation.frame = true;
  const auto diff = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight, invalidation,
      [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); });
  Expect(diff.identical(), "a blink in the lower pane should paint what a full repaint paints; " +
                               DescribePixelDiff(diff));
}

void TestWorkspaceShellTerminalFindBarOwnsTheBlink() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the terminal fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  auto& session = WorkspaceShellTestAccess::ActiveTerminalSession(shell);
  TerminalSessionTestAccess::Reset(session, 8, 80);
  TerminalSessionTestAccess::AppendOutput(session, "prompt$ ");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const Uint64 hidden = HiddenCaretBlinkEpoch(shell);

  SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)DispatchEvents(shell, {KeyDownEvent(SDLK_F, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT))});
  Expect(WorkspaceShellTestAccess::CurrentTextInputSurface(shell) ==
             microide::workspace::TextInputSurface::TerminalFind,
         "Ctrl+Shift+F in the terminal should focus its find bar");
  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, hidden);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks());
  const std::optional<SDL_FRect> blink = shell.CurrentCaretDirtyRect();
  Expect(blink.has_value(), "the find field should name its caret rect");
  WorkspaceShell::RenderInvalidation invalidation;
  invalidation.rects.push_back(*blink);
  const auto diff = PaintAndDiffWithFullRepaint(
      shell, canvas, kWidth, kHeight, invalidation,
      [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); });
  Expect(diff.identical(), "a find-bar blink should paint what a full repaint paints; " +
                               DescribePixelDiff(diff));
}

void TestWorkspaceShellCaretBlinkIsDecidedOncePerFrame() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1024;
  static constexpr int kHeight = 640;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the caret fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  WorkspaceShellTestAccess::OpenCommandPalette(shell);
  shell.Render(canvas.renderer(), kWidth, kHeight);

  const Uint64 hidden_epoch = HiddenCaretBlinkEpoch(shell);

  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks());
  Expect(WorkspaceShellTestAccess::CaretVisibleNow(shell), "a fresh epoch shows the caret");
  const WorkspaceShell::FrameToken token = shell.PrepareFrameOnce(canvas.renderer(), kWidth, kHeight);
  shell.RenderClip(token, canvas.renderer(), kWidth, kHeight);
  SDL_FlushRenderer(canvas.renderer());
  SDL_Surface* first = SDL_DuplicateSurface(canvas.surface());

  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, hidden_epoch);
  shell.RenderClip(token, canvas.renderer(), kWidth, kHeight);
  SDL_FlushRenderer(canvas.renderer());
  const std::size_t differences = CountPixelDifferences(first, canvas.surface());
  SDL_DestroySurface(first);
  Expect(differences == 0, "two renders of one prepared frame should paint the same caret; " +
                               std::to_string(differences) + " pixels differ");
}

void TestWorkspaceShellOpenedPaletteShowsItsCaretOnTheFirstFrame() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1024;
  static constexpr int kHeight = 640;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the palette fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);

  WorkspaceShellTestAccess::OpenCommandPalette(shell);
  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks());
  shell.Render(canvas.renderer(), kWidth, kHeight);
  SDL_FlushRenderer(canvas.renderer());
  SDL_Surface* first = SDL_DuplicateSurface(canvas.surface());
  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks());
  shell.Render(canvas.renderer(), kWidth, kHeight);
  SDL_FlushRenderer(canvas.renderer());
  const std::size_t differences = CountPixelDifferences(first, canvas.surface());
  SDL_DestroySurface(first);
  Expect(differences == 0, "the palette's first frame should paint what its second paints; " +
                               std::to_string(differences) + " pixels differ");
}

void TestWorkspaceShellUnfocusedEditorDoesNotPaintTheBlinkingCaret() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "alpha.cpp", "int value = 1;\n");

  static constexpr int kWidth = 1024;
  static constexpr int kHeight = 640;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the caret fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, root / "alpha.cpp");
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "");
  Expect(WorkspaceShellTestAccess::CurrentTextInputSurface(shell) ==
             microide::workspace::TextInputSurface::SidebarSearchQuery,
         "the search view's query field should own the keyboard");

  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, HiddenCaretBlinkEpoch(shell));
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks());
  const std::optional<SDL_FRect> blink = shell.CurrentCaretDirtyRect();
  Expect(blink.has_value(), "the focused query field should name its caret rect");
  WorkspaceShell::RenderInvalidation invalidation;
  invalidation.rects.push_back(*blink);
  PaintInvalidation(shell, canvas, kWidth, kHeight, invalidation);
  SDL_FlushRenderer(canvas.renderer());
  SDL_Surface* retained = SDL_DuplicateSurface(canvas.surface());
  shell.Render(canvas.renderer(), kWidth, kHeight);
  SDL_FlushRenderer(canvas.renderer());
  const std::size_t differences = CountPixelDifferences(retained, canvas.surface());
  SDL_DestroySurface(retained);
  Expect(differences == 0, "a blink repaint should leave what a full repaint paints; " +
                               std::to_string(differences) + " pixels differ");
}

void TestWorkspaceShellEditorRepaintUnderAnOpenMenuKeepsTheEditor() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "wide.cpp";
  std::string body;
  for (int line = 0; line < 30; ++line) {
    body += "int value_" + std::to_string(line) +
            " = compute_something_rather_long(alpha, beta, gamma, delta, epsilon, zeta);\n";
  }
  WriteFile(source, body);

  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the menu repaint fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::NoteLayoutInputsChanged(shell);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };

  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  WorkspaceShellTestAccess::OpenMenuBarMenuForTest(shell, WorkspaceShell::MenuId::Edit);
  (void)WorkspaceShellTestAccess::TakePendingRenderInvalidation(shell);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const std::optional<SDL_FRect> popup =
      WorkspaceShellTestAccess::PopupMenuRectForTest(shell, WorkspaceShell::MenuId::Edit);
  Expect(popup.has_value(), "the Edit menu should be open");
  const float left = popup->x + popup->w + 24.0f;
  const SDL_FRect damage{left, layout.editor_surface.y + 6.0f,
                         layout.editor_surface.x + layout.editor_surface.w - left - 24.0f, 14.0f};
  Expect(damage.w > 100.0f && damage.y + damage.h <= popup->y + popup->h,
         "the fixture's damage must sit inside the old bounding box, or it proves nothing");

  WorkspaceShellTestAccess::QueueUnrelatedDamage(shell, damage);
  pin_caret();
  PaintInvalidation(shell, canvas, kWidth, kHeight,
                    WorkspaceShellTestAccess::TakePendingRenderInvalidation(shell));
  SDL_FlushRenderer(canvas.renderer());
  SDL_Surface* retained = SDL_DuplicateSurface(canvas.surface());
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  SDL_FlushRenderer(canvas.renderer());
  const std::size_t differences = CountPixelDifferences(retained, canvas.surface());
  SDL_DestroySurface(retained);
  Expect(differences == 0,
         "an editor repaint under an open menu should match a full repaint; " +
             std::to_string(differences) + " pixels differ");
}

void TestWorkspaceShellProjectTabTooltipDismissRetainedRedrawMatchesFullRender() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
#if defined(__SANITIZE_THREAD__)
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
  PaintInvalidation(retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight,
                             hover_result.redraw);
  Expect(WorkspaceShellTestAccess::HoveredTooltipRect(retained_shell).has_value(),
         "hovering a project tab should expose a tooltip rect");

  const SDL_FRect editor_rect = WorkspaceShellTestAccess::ActiveEditorPaneRect(retained_shell);
  SDL_Event dismiss_event{};
  dismiss_event.type = SDL_EVENT_MOUSE_MOTION;
  dismiss_event.motion.x = editor_rect.x + 20.0f;
  dismiss_event.motion.y = editor_rect.y + 20.0f;
  const auto dismiss_result = retained_shell.HandleEvent(dismiss_event);
  Expect(dismiss_result.handled,
         "moving away from a project tab tooltip should stay on a handled redraw path");
  PaintInvalidation(retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight,
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

void TestWorkspaceShellSettingsRowsLiftOnHover() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "file.txt", "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);
  WorkspaceShellTestAccess::SetSettingsOverlayCategory(shell, 0);
  WorkspaceShellTestAccess::SetSettingsOverlaySelectedRow(shell, 0);

  const SDL_FRect row = WorkspaceShellTestAccess::SettingsOverlayVisibleRowRect(shell, 1);
  Expect(row.w > 0.0f && row.h > 0.0f, "the settings fixture should expose a second value row");
  Expect(WorkspaceShellTestAccess::SettingsOverlaySelectedRowIndex(shell) == 0,
         "the settings fixture should keep the selection off the hovered row");

  const auto theme = microide::render::MakeDefaultTheme();
  const auto row_pixel = [&](float x, float y) {
    SoftwareCanvas canvas(1280, 720);
    shell.Render(canvas.renderer(), 1280, 720);
    SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
    Expect(pixels != nullptr, "the settings hover fixture should capture rendered pixels");
    const SDL_Color color = ReadSurfacePixelOrThrow(pixels, static_cast<int>(std::floor(x)),
                                                    static_cast<int>(std::floor(y)));
    SDL_DestroySurface(pixels);
    return color;
  };
  const auto is_row_highlight = [&](const SDL_Color& color) {
    return color.r == theme.row_highlight.r && color.g == theme.row_highlight.g &&
           color.b == theme.row_highlight.b && color.a == theme.row_highlight.a;
  };
  const float sample_x = row.x + 3.0f;
  const float sample_y = row.y + row.h * 0.5f;

  (void)SendMouseMotion(shell, row.x + row.w * 0.5f, row.y - 1000.0f, 0);
  Expect(!is_row_highlight(row_pixel(sample_x, sample_y)),
         "an unhovered, unselected settings row should keep the pane background");

  (void)SendMouseMotion(shell, row.x + row.w * 0.5f, sample_y, 0);
  Expect(is_row_highlight(row_pixel(sample_x, sample_y)),
         "the settings row under the pointer should lift like every other list row");
}

void TestWorkspaceShellPluginSurfacePanelDrawsItsFocusRing() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "file.txt", "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  state.panel.content = WorkspaceShell::PanelContentKind::PluginSurface;
  state.panel.surface_owner = "plug";
  state.panel.surface_id = "chart";

  const auto theme = microide::render::MakeDefaultTheme();
  const auto is_accent = [&](const SDL_Color& color) {
    return color.r == theme.accent.r && color.g == theme.accent.g &&
           color.b == theme.accent.b && color.a == theme.accent.a;
  };
  const auto ring_pixel = [&]() {
    const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
    SoftwareCanvas canvas(1280, 720);
    shell.Render(canvas.renderer(), 1280, 720);
    SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
    Expect(pixels != nullptr, "the panel focus fixture should capture rendered pixels");
    const SDL_Color color = ReadSurfacePixelOrThrow(
        pixels, static_cast<int>(std::floor(layout.bottom_panel.x + 1.0f)),
        static_cast<int>(std::floor(layout.bottom_panel.y + layout.bottom_panel.h * 0.5f)));
    SDL_DestroySurface(pixels);
    return color;
  };

  WorkspaceShellTestAccess::SetFocusEditor(shell);
  Expect(!is_accent(ring_pixel()),
         "an unfocused plugin surface panel should not draw a focus ring");
  WorkspaceShellTestAccess::SetFocusPanel(shell);
  Expect(is_accent(ring_pixel()),
         "a focused plugin surface panel should draw the same focus ring as every other surface");
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
  SoftwareCanvas cut_shell_canvas{1280, 720};
  cut_shell.Render(cut_shell_canvas.renderer(), 1280, 720);
  (void)cut_shell.ConsumePendingRenderInvalidation();

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
  const auto select_all_damage = FrameDamage(cut_shell, cut_shell_canvas, 1280, 720, select_all.redraw);
  Expect(select_all.handled, "Ctrl+A should be handled by the editor");
  Expect(!select_all_damage.full && !select_all_damage.rects.empty(),
         "Ctrl+A should request a partial editor redraw");
  Expect(AnyRectIntersects(select_all_damage.rects, cut_editor_surface),
         "Ctrl+A redraws should include the editor surface");
  Expect(WorkspaceShellTestAccess::ActiveEditorHasSelection(cut_shell),
         "Ctrl+A should select the active editor contents");

  const auto cut = handle_shortcut(cut_shell, SDLK_X);
  const auto cut_damage = FrameDamage(cut_shell, cut_shell_canvas, 1280, 720, cut.redraw);
  Expect(cut.handled, "Ctrl+X should be handled by the editor");
  Expect(!cut_damage.full && !cut_damage.rects.empty(),
         "Ctrl+X should request a partial editor redraw");
  Expect(AnyRectIntersects(cut_damage.rects, cut_editor_surface),
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
  SoftwareCanvas copy_line_shell_canvas{1280, 720};
  copy_line_shell.Render(copy_line_shell_canvas.renderer(), 1280, 720);
  (void)copy_line_shell.ConsumePendingRenderInvalidation();
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
  const auto copy_line_damage = FrameDamage(copy_line_shell, copy_line_shell_canvas, 1280, 720, copy_line.redraw);
  Expect(copy_line.handled, "Ctrl+C without a selection should still be handled by the editor");
  Expect(clipboard_text == "beta\n",
         "Ctrl+C without a selection should copy the full active line");

  configure_shell(cut_line_shell);
  SoftwareCanvas cut_line_shell_canvas{1280, 720};
  cut_line_shell.Render(cut_line_shell_canvas.renderer(), 1280, 720);
  (void)cut_line_shell.ConsumePendingRenderInvalidation();
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
  const auto cut_line_damage = FrameDamage(cut_line_shell, cut_line_shell_canvas, 1280, 720, cut_line.redraw);
  Expect(cut_line.handled, "Ctrl+X without a selection should still be handled by the editor");
  Expect(clipboard_text == "beta\n",
         "Ctrl+X without a selection should copy the full active line");
  Expect(WorkspaceShellTestAccess::ActiveEditor(cut_line_shell).lines().Snapshot() ==
             std::vector<std::string>({"alpha", "gamma"}),
         "Ctrl+X without a selection should remove the full active line");

  WorkspaceShell undo_shell;
  configure_shell(undo_shell);
  SoftwareCanvas undo_shell_canvas{1280, 720};
  undo_shell.Render(undo_shell_canvas.renderer(), 1280, 720);
  (void)undo_shell.ConsumePendingRenderInvalidation();

  SDL_Event text_event{};
  text_event.type = SDL_EVENT_TEXT_INPUT;
  const std::string typed_text = "x";
  text_event.text.text = typed_text.c_str();
  const auto typed = undo_shell.HandleEvent(text_event);
  Expect(typed.handled, "undo invalidation fixture should type into the editor first");
  (void)FrameDamage(undo_shell, undo_shell_canvas, 1280, 720, typed.redraw);
  const SDL_FRect undo_editor_surface = WorkspaceShellTestAccess::CurrentLayout(undo_shell).editor_surface;
  const auto undo = handle_shortcut(undo_shell, SDLK_Z);
  const auto undo_damage = FrameDamage(undo_shell, undo_shell_canvas, 1280, 720, undo.redraw);
  Expect(undo.handled, "Ctrl+Z should be handled by the editor");
  Expect(!undo_damage.full && !undo_damage.rects.empty(),
         "Ctrl+Z should request a partial editor redraw");
  Expect(AnyRectIntersects(undo_damage.rects, undo_editor_surface),
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

void TestWorkspaceShellSecondarySidebarsDoNotPaintTheFileTree() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "alpha.cpp";
  WriteFile(source, "int alpha() { return 1; }\n");
  WriteFile(root / "src" / "beta.cpp", "int beta() { return 2; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const auto body_pixels = [&]() {
    const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
    const float body_top = WorkspaceShellTestAccess::ProblemsSidebarRowRect(shell, 0).y;
    SoftwareCanvas canvas(1280, 720);
    shell.Render(canvas.renderer(), 1280, 720);
    SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
    Expect(pixels != nullptr, "the sidebar fixture should capture rendered pixels");
    std::vector<SDL_Color> sample;
    for (int y = static_cast<int>(body_top);
         y < static_cast<int>(layout.sidebar.y + layout.sidebar.h); ++y) {
      for (int x = static_cast<int>(layout.sidebar.x);
           x < static_cast<int>(layout.sidebar.x + layout.sidebar.w); ++x) {
        sample.push_back(ReadSurfacePixelOrThrow(pixels, x, y));
      }
    }
    SDL_DestroySurface(pixels);
    return sample;
  };
  const auto same = [](const std::vector<SDL_Color>& lhs, const std::vector<SDL_Color>& rhs) {
    if (lhs.size() != rhs.size()) {
      return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
      if (lhs[i].r != rhs[i].r || lhs[i].g != rhs[i].g || lhs[i].b != rhs[i].b ||
          lhs[i].a != rhs[i].a) {
        return false;
      }
    }
    return true;
  };

  WorkspaceShellTestAccess::ShowProblemsSidebar(shell);
  const std::vector<SDL_Color> problems_empty = body_pixels();

  Expect(WorkspaceShellTestAccess::PublishDiagnostics(
             shell, "diagnostics", source,
             {microide::editor::Diagnostic{
                 .range =
                     microide::editor::SelectionRange{
                         .start = microide::editor::TextPosition{.line = 0, .column = 0},
                         .end = microide::editor::TextPosition{.line = 0, .column = 3},
                     },
                 .severity = microide::editor::DiagnosticSeverity::Error,
                 .message = "problems row must be painted",
             }}),
         "the Problems fixture should publish one diagnostic");
  Expect(WorkspaceShellTestAccess::RefreshProblemsSidebar(shell),
         "the Problems sidebar should pick the published diagnostic up");
  Expect(!WorkspaceShellTestAccess::ProblemsSidebarEntries(shell).empty(),
         "the Problems sidebar should hold an entry to paint");
  Expect(!same(problems_empty, body_pixels()),
         "the Problems sidebar must paint its own rows, not the file tree's");

  WorkspaceShellTestAccess::ShowTestsSidebar(shell);
  const std::vector<SDL_Color> tests_body = body_pixels();
  WorkspaceShellTestAccess::ShowProblemsSidebar(shell);
  Expect(!same(tests_body, body_pixels()),
         "the Tests and Problems sidebars must paint their own rows, not one shared tree");
}

void TestWorkspaceShellCallHierarchyIsGatedLikeItsSiblings() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "alpha.cpp";
  WriteFile(source, "int alpha() { return 1; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::CallHierarchy),
         "Call Hierarchy should be disabled with no editor open");

  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  Expect(WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::CallHierarchy),
         "Call Hierarchy should be enabled for a saved buffer");

  WorkspaceShellTestAccess::SetSettingValue(shell, "lsp.call_hierarchy.enabled", "false");
  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::CallHierarchy),
         "turning off lsp.call_hierarchy.enabled should disable Call Hierarchy");
  WorkspaceShellTestAccess::SetSettingValue(shell, "lsp.call_hierarchy.enabled", "true");
  WorkspaceShellTestAccess::SetSettingValue(shell, "lsp.enabled", "false");
  Expect(!WorkspaceShellTestAccess::IsActionEnabled(shell, ActionId::CallHierarchy),
         "the LSP master switch should disable Call Hierarchy too");
}

void TestWorkspaceShellProjectTabContextMenuCopiesProjectRoot() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path target = root / "src" / "alpha.cpp";
  WriteFile(target, "int alpha() { return 1; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
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

  WorkspaceShellTestAccess::OpenTreeContextMenuForPath(
      shell, TreeContextTargetKind::File, file);
  Expect(WorkspaceShellTestAccess::ExecuteShowInFileExplorer(shell),
         "Show in File Explorer should execute from the tree context menu");
  Expect(revealed == nested_dir.lexically_normal(),
         ("Show in File Explorer should open the file's containing directory "
          "(actual: " +
          revealed.string() + ")")
             .c_str());

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

void TestWorkspaceShellTreeContextMenuHoverMatchesTheRowBuilder() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "src" / "alpha.cpp";
  WriteFile(file, "int alpha() { return 1; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenTreeContextMenuForPath(shell, TreeContextTargetKind::File, file);

  const auto popup_rect = WorkspaceShellTestAccess::TreeContextMenuRect(shell);
  Expect(popup_rect.has_value(), "the tree context menu should have a popup rect once open");
  const auto rows = WorkspaceShellTestAccess::TreeContextMenuVisibleItems(shell);
  Expect(rows.size() >= 3, "the file context menu should paint several rows");

  int hovered_rows = 0;
  int separator_or_disabled_rows = 0;
  for (const auto& row : rows) {
    Expect(SendMouseMotion(shell, row.rect.x + row.rect.w * 0.5f, row.rect.y + row.rect.h * 0.5f, 0),
           "a motion inside the open context menu should be handled");
    const int active = WorkspaceShellTestAccess::TreeContextMenuActiveItemIndex(shell);
    const int expected = row.enabled ? static_cast<int>(row.index) : -1;
    Expect(active == expected,
           "hovering row " + std::to_string(row.index) + " should leave the active index at " +
               std::to_string(expected) + ", got " + std::to_string(active));
    if (row.enabled) {
      ++hovered_rows;
    } else {
      ++separator_or_disabled_rows;
    }
  }
  Expect(hovered_rows > 0, "no row in the file context menu was enabled");
  Expect(separator_or_disabled_rows > 0,
         "the file context menu had no separator or disabled row to check the -1 branch with");

  Expect(SendMouseMotion(shell, popup_rect->x + 1.0f, popup_rect->y + 1.0f, 0),
         "a motion in the popup padding should be handled");
  Expect(WorkspaceShellTestAccess::TreeContextMenuActiveItemIndex(shell) == -1,
         "hovering the popup padding should light no row");

  const auto enabled = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
    return row.enabled && !row.separator;
  });
  Expect(enabled != rows.end(), "the file context menu should have an enabled row");
  Expect(SendMouseDown(shell, enabled->rect.x + enabled->rect.w * 0.5f,
                       enabled->rect.y + enabled->rect.h * 0.5f, SDL_BUTTON_LEFT),
         "clicking an enabled context menu row should be handled");
}

void TestWorkspaceShellRevealInFileExplorerValidatesPathSynchronously() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "a.txt", "a\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  Expect(!WorkspaceShellTestAccess::RevealPathInFileExplorer(shell, {}),
         "an empty reveal path should fail synchronously");
  Expect(!WorkspaceShellTestAccess::RevealPathInFileExplorer(shell, root / "does-not-exist"),
         "a missing reveal path should fail synchronously without dispatching a subprocess");

  bool opened = false;
  WorkspaceShellTestAccess::SetFileManagerOpener(
      shell, [&](const std::filesystem::path&) {
        opened = true;
        return true;
      });
  Expect(WorkspaceShellTestAccess::RevealPathInFileExplorer(shell, root),
         "a valid reveal with an injected opener should report success");
  Expect(opened, "the injected opener should have been invoked");
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
  WorkspaceShellTestAccess::SettleGitSidebarRefresh(retained_shell);
  const auto redraw = FrameDamage(retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight,
                                  retained_shell.ConsumePendingRenderInvalidation());
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(retained_shell);
  Expect(!redraw.full && !redraw.rects.empty(),
         "switching sidebar modes without geometry changes should stay on partial redraws");
  Expect(AnyRectIntersects(redraw.rects, layout.sidebar),
         "switching sidebar modes should invalidate the sidebar surface");

  PaintInvalidation(retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight, redraw);

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
    Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
           "tab redraw regression fixture should open its project");
    WorkspaceShellTestAccess::SetWindowSize(shell, kCanvasWidth, kCanvasHeight);
    WorkspaceShellTestAccess::OpenSingleEditorTab(shell, alpha);
    (void)shell.ConsumePendingRenderInvalidation();
  };

  WorkspaceShell retained_shell;
  configure_shell(retained_shell);

  SoftwareCanvas retained_canvas(kCanvasWidth, kCanvasHeight);
  retained_shell.Render(retained_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(retained_shell, beta),
         "tab redraw regression fixture should open a second file");
  const auto redraw = FrameDamage(retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight,
                                  retained_shell.ConsumePendingRenderInvalidation());
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(retained_shell);
  Expect(!redraw.full && !redraw.rects.empty(),
         "opening a file in a new tab should stay on partial redraws");
  Expect(AnyRectIntersects(redraw.rects, layout.breadcrumb),
         "opening a file in a new tab should invalidate the breadcrumb");
  Expect(AnyRectIntersects(redraw.rects, layout.tab_strip),
         "opening a file in a new tab should invalidate the tab strip");
  Expect(AnyRectIntersects(redraw.rects, layout.editor_surface),
         "opening a file in a new tab should invalidate the editor surface");

  PaintInvalidation(retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight, redraw);

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
  const auto redraw =
      FrameDamage(shell, canvas, kCanvasWidth, kCanvasHeight, shell.ConsumePendingRenderInvalidation());
  Expect(!redraw.full && !redraw.rects.empty(),
         "compare-gate redraw fixture should produce a partial redraw");

  PaintInvalidation(shell, canvas, kCanvasWidth, kCanvasHeight, redraw);
  Expect(WorkspaceShellTestAccess::RenderCompareSurfaceInvocationCount(shell) == 0,
         "partial redraw with no compare tab active should not call RenderCompareSurface");
}

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
  Expect(WorkspaceShellTestAccess::SetSettingValueTransient(shell, "editor.font_family",
                                                            "HeapFontName1234"),
         "the font-family String setting should accept an arbitrary value");
  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);
  WorkspaceShellTestAccess::SetSettingsOverlayQueryAndRefresh(shell, "font family");

  SoftwareCanvas canvas(kCanvasWidth, kCanvasHeight);
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

  SoftwareCanvas canvas{1280, 720};
  shell.Render(canvas.renderer(), 1280, 720);
  (void)shell.ConsumePendingRenderInvalidation();
  const SDL_FRect editor_surface = WorkspaceShellTestAccess::CurrentLayout(shell).editor_surface;
  const std::size_t before_line_count =
      WorkspaceShellTestAccess::ActiveEditor(shell).line_count();
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = SDLK_RETURN;
  const auto result = shell.HandleEvent(event);
  const auto damage = FrameDamage(shell, canvas, 1280, 720, result.redraw);
  Expect(result.handled,
         "newline redraw regression fixture should insert a newline into the editor");
  Expect(!damage.full && !damage.rects.empty(),
         "newline insertion should request a partial editor redraw after changing the editor line count");
  Expect(AnyRectIntersects(damage.rects, editor_surface),
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
  WorkspaceShellTestAccess::SetFocusSidebar(retained_shell);
  WorkspaceShellTestAccess::SetFocusSidebar(reference_shell);

  PaintInvalidation(retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight,
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

  PaintInvalidation(
      retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight, retained_drag.redraw);
  Expect(!retained_shell.ConsumePostRenderRedrawRequest().has_value(),
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

  PaintInvalidation(retained_shell, retained_canvas, kCanvasWidth, kCanvasHeight, redraw);
  const auto first_settle = retained_shell.ConsumePostRenderRedrawRequest();
  Expect(first_settle.has_value(),
         "bottom-panel resize should schedule a follow-up redraw after the first full render");
  Expect(!first_settle->full && first_settle->rects.size() == 1,
         "the settle redraw should be a single rect, not a whole-window repaint");
  const SDL_FRect settle_rect = first_settle->rects.front();
  Expect(settle_rect.x == resized_layout.bottom_panel.x &&
             settle_rect.y == resized_layout.bottom_panel.y &&
             settle_rect.w == resized_layout.bottom_panel.w &&
             settle_rect.h == resized_layout.bottom_panel.h,
         "the settle redraw rect should be the bottom panel");
  retained_shell.Render(retained_canvas.renderer(), kCanvasWidth, kCanvasHeight);
  Expect(retained_shell.ConsumePostRenderRedrawRequest().has_value(),
         "bottom-panel resize should schedule a second settle redraw");
  retained_shell.Render(retained_canvas.renderer(), kCanvasWidth, kCanvasHeight);
  Expect(!retained_shell.ConsumePostRenderRedrawRequest().has_value(),
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

void TestWorkspaceShellLayoutMemoTracksEveryInput() {
  using microide::workspace::ComputeLayout;
  using microide::workspace::LayoutModeInputs;
  using microide::workspace::WorkspaceLayout;
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path first = temp_dir.path() / "first";
  const std::filesystem::path second = temp_dir.path() / "second";
  std::filesystem::create_directories(first);
  std::filesystem::create_directories(second);

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, first, false, false),
         "the memo fixture needs a project so the project tab strip has a state to flip");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetSidebarVisible(shell, true);
  WorkspaceShellTestAccess::SetStatusBarVisible(shell, true);

  WorkspaceLayout previous = WorkspaceShellTestAccess::CurrentLayout(shell);
  auto expect_moved = [&](const char* what) {
    const WorkspaceLayout memo = WorkspaceShellTestAccess::CurrentLayout(shell);
    const WorkspaceLayout fresh = ComputeLayout(WorkspaceShellTestAccess::CurrentLayoutInputs(shell));
    Expect(memo == fresh, std::string("after ") + what +
                              " the memo should answer what a fresh compute of the state answers");
    Expect(memo != previous, std::string(what) + " should move the layout, or this case proves nothing");
    previous = memo;
  };

  util::ResetPerformanceCounters();
  for (int i = 0; i < 10; ++i) {
    (void)WorkspaceShellTestAccess::CurrentLayout(shell);
  }
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::WorkspaceLayoutComputes) == 0,
         "ten queries against unchanged state should compute nothing");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::WorkspaceLayoutMemoHits) == 10,
         "ten queries against unchanged state should be ten memo hits");

  WorkspaceShellTestAccess::SetWindowSize(shell, 1400, 900);
  expect_moved("a window resize");
  WorkspaceShellTestAccess::SetSidebarWidth(shell, 320.0f);
  expect_moved("a sidebar width change");
  WorkspaceShellTestAccess::SetSidebarVisible(shell, false);
  expect_moved("hiding the sidebar");
  if (WorkspaceShellTestAccess::BottomPanelVisible(shell)) {
    WorkspaceShellTestAccess::HideBottomPanel(shell);
    expect_moved("hiding the bottom panel");
  }
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  expect_moved("showing the bottom panel");
  WorkspaceShellTestAccess::SetBottomPanelHeight(shell,
                                                 WorkspaceShellTestAccess::BottomPanelHeight(shell) + 40.0f);
  expect_moved("a bottom panel height change");
  WorkspaceShellTestAccess::SetStatusBarVisible(shell, false);
  expect_moved("hiding the status bar");
  WorkspaceShellTestAccess::SetDebugPaneVisible(shell, true);
  expect_moved("showing the debug pane");
  WorkspaceShellTestAccess::SetDebugPaneWidth(shell, 400.0f);
  expect_moved("a debug pane width change");
  WorkspaceShellTestAccess::SetLayoutModeOverride(shell, LayoutModeInputs::Override::Compact);
  expect_moved("forcing the compact layout mode");
  WorkspaceShellTestAccess::SetLayoutModeOverride(shell, LayoutModeInputs::Override::Auto);
  expect_moved("releasing the layout mode override");
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, second, false, false),
         "the second project should open");
  expect_moved("a second project tab (the project tab strip appears)");

  SoftwareCanvas canvas(1400, 900);
  shell.Render(canvas.renderer(), 1400, 900);
  WorkspaceShellTestAccess::SetSidebarVisible(shell, true);
  (void)WorkspaceShellTestAccess::CurrentLayout(shell);
  util::ResetPerformanceCounters();
  shell.Render(canvas.renderer(), 1400, 900);
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::WorkspaceLayoutComputes) == 0,
         "frame prep should serve its layout from the memo the hit-test query just filled");
}

void TestWorkspaceShellEditorGroupRectsMemoFollowsTheTree() {
  using microide::workspace::EditorGroupRectsLayout;
  using microide::workspace::EditorSplitOrientation;
  using microide::workspace::WorkspaceLayout;
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.txt";
  WriteFile(alpha, "alpha\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the pane-rect memo fixture needs a project");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, alpha);

  const WorkspaceLayout layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  auto rects_equal = [](const SDL_FRect& a, const SDL_FRect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
  };
  auto rects_after = [&](const WorkspaceLayout& for_layout, bool require_miss, const char* what) {
    util::ResetPerformanceCounters();
    const EditorGroupRectsLayout first = WorkspaceShellTestAccess::EditorGroupRectsFor(shell, for_layout);
    const EditorGroupRectsLayout again = WorkspaceShellTestAccess::EditorGroupRectsFor(shell, for_layout);
    const std::uint64_t builds =
        util::ReadPerformanceCounter(util::PerfCounterId::WorkspaceEditorGroupRectBuilds);
    const std::uint64_t hits =
        util::ReadPerformanceCounter(util::PerfCounterId::WorkspaceEditorGroupRectMemoHits);
    if (require_miss) {
      Expect(builds == 1, std::string(what) + ": the first ask should be exactly one walk");
    }
    Expect(builds + hits == 2 && hits >= 1,
           std::string(what) + ": the second identical ask should be a memo hit");
    Expect(first.groups.size() == again.groups.size() && first.dividers.size() == again.dividers.size(),
           std::string(what) + ": the memo answer should match the walk it replaced");
    for (std::size_t i = 0; i < first.groups.size(); ++i) {
      Expect(rects_equal(first.groups[i].editor_surface, again.groups[i].editor_surface),
             std::string(what) + ": memoized pane rects should be byte-identical to the walk");
    }
    return first;
  };

  const EditorGroupRectsLayout single = rects_after(layout, false, "single pane, first ask");
  Expect(single.groups.size() == 1, "one pane before the split");

  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "the fixture needs a split");
  const EditorGroupRectsLayout split = rects_after(layout, false, "after the split");
  Expect(split.groups.size() == 2 && split.dividers.size() == 1,
         "after the split the memo must report two panes and a divider");

  WorkspaceLayout wider = layout;
  wider.editor_area.w += 200.0f;
  const EditorGroupRectsLayout for_wider = rects_after(wider, true, "a different layout value");
  Expect(for_wider.groups[1].editor_surface.x + for_wider.groups[1].editor_surface.w >
             split.groups[1].editor_surface.x + split.groups[1].editor_surface.w,
         "a wider editor area should push the right pane's edge out");

  const EditorGroupRectsLayout back = rects_after(layout, true, "back to the first layout");
  Expect(rects_equal(back.groups[1].editor_surface, split.groups[1].editor_surface),
         "returning to a layout should recompute it, not serve the other layout's rects");

  Expect(WorkspaceShellTestAccess::ResizeRootSplitDivider(shell, 0, 0.3f),
         "the fixture needs a divider move");
  const EditorGroupRectsLayout moved = rects_after(layout, true, "after a divider move");
  Expect(moved.groups[0].editor_surface.w < back.groups[0].editor_surface.w,
         "a divider moved left should shrink the left pane in the memo's next answer");
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

void TestWorkspaceShellFoldingRefreshRunsOncePerPreparedFrame() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(1280, 720);

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "main.cpp";
  WriteFile(file, "void f() {\n  body();\n  more();\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "folding-refresh regression test should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, file);

  shell.Render(canvas.renderer(), 1280, 720);
  util::ResetPerformanceCounters();

  shell.PrepareRenderFrame(canvas.renderer(), 1280, 720);
  Expect(util::ReadPerformanceCounter(
             util::PerfCounterId::FrameRefreshEditorFoldingModelsCalls) == 1,
         "PrepareFrameOnce should resolve editor folding exactly once per prepared frame");

  Expect(!WorkspaceShellTestAccess::EnsureActiveFoldingModelFresh(shell)->resolved_ranges().empty(),
         "prepared-frame folding refresh should resolve the fold range before render");

  for (int i = 0; i < 5; ++i) {
    shell.RenderPrepared(canvas.renderer(), 1280, 720);
  }
  Expect(util::ReadPerformanceCounter(
             util::PerfCounterId::FrameRefreshEditorFoldingModelsCalls) == 1,
         "RenderClip partial redraws must not re-run the folding refresh");
}

void TestWorkspaceShellFramePrepCountersRunOncePerPreparedFrame() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(1280, 720);

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path file = root / "main.txt";
  WriteFile(file, "alpha\nbeta\ngamma\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "frame-prep counter regression test should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, file);

  shell.Render(canvas.renderer(), 1280, 720);
  util::ResetPerformanceCounters();

  shell.PrepareRenderFrame(canvas.renderer(), 1280, 720);
  for (int i = 0; i < 5; ++i) {
    shell.RenderPrepared(canvas.renderer(), 1280, 720);
  }

  Expect(util::ReadPerformanceCounter(util::PerfCounterId::FramePrepareCalls) == 1,
         "retained partial redraws must not re-run PrepareFrameOnce");
  Expect(util::ReadPerformanceCounter(util::PerfCounterId::FrameRefreshStatusBarCalls) == 1,
         "the status-bar model must rebuild exactly once per prepared frame, not per clip");
  Expect(util::ReadPerformanceCounter(
             util::PerfCounterId::RenderViewModelBuildFrameSurfaceCalls) == 1,
         "the frame-surface view model must build exactly once per prepared frame");
  Expect(util::ReadPerformanceCounter(
             util::PerfCounterId::RenderViewModelBuildOverlaySurfaceCalls) == 1,
         "the overlay-surface view model must build exactly once per prepared frame");
  Expect(util::ReadPerformanceCounter(
             util::PerfCounterId::FrameApplyEditorPreferencesAllTabsCalls) == 0,
         "the all-tabs preference apply is event-driven and must not run on quiet frames");
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

  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  const auto overflow_at_left =
      WorkspaceShellTestAccess::EditorTabOverflowControls(shell);
  Expect(overflow_at_left.hidden_left == 0,
         "no tabs should be hidden left when the first tab is active");
  Expect(overflow_at_left.hidden_right > 0,
         "tabs should be hidden right when more open than fit");

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

void TestEditorWheelScrollsHorizontally() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path wide = root / "wide.txt";
  WriteFile(wide, std::string(4000, 'x') + "\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, wide);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const float wheel_x = layout.editor_surface.x + layout.editor_surface.w * 0.5f;
  const float wheel_y = layout.editor_surface.y + layout.editor_surface.h * 0.5f;

  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).horizontal_scroll() == 0,
         "the editor should start unscrolled horizontally");
  const std::size_t scroll_line_before =
      WorkspaceShellTestAccess::ActiveEditor(shell).scroll_line();
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, 0, -2),
         "a horizontal wheel over the editor should be handled");
  const std::size_t scrolled = WorkspaceShellTestAccess::ActiveEditor(shell).horizontal_scroll();
  Expect(scrolled > 0, "a horizontal wheel should scroll the editor sideways");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).scroll_line() == scroll_line_before,
         "a horizontal wheel must not scroll the editor vertically");

  Expect(SendMouseWheel(shell, wheel_x, wheel_y, 0, 2), "the reverse direction is handled too");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).horizontal_scroll() < scrolled,
         "wheeling back should reduce the horizontal scroll");
}

void TestEditorTabSwitchAcceptsRelativeOffsetsAndKeyChords() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "tab switch\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  for (int i = 0; i < 3; ++i) {
    const std::filesystem::path file = root / ("switch_" + std::to_string(i) + ".txt");
    WriteFile(file, "alpha\n");
    WorkspaceShellTestAccess::OpenFile(shell, file);
  }
  const std::size_t tab_count = WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell);
  Expect(tab_count >= 3, "fixture should open at least three tabs");

  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  Expect(SendKeyDown(shell, SDLK_PAGEDOWN, SDL_KMOD_CTRL),
         "Ctrl+PageDown should be consumed by the editor");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 1,
         "Ctrl+PageDown should activate the next tab");
  Expect(SendKeyDown(shell, SDLK_PAGEUP, SDL_KMOD_CTRL),
         "Ctrl+PageUp should be consumed by the editor");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 0,
         "Ctrl+PageUp should activate the previous tab");

  Expect(SendKeyDown(shell, SDLK_PAGEUP, SDL_KMOD_CTRL), "Ctrl+PageUp at the first tab is handled");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == tab_count - 1,
         "Ctrl+PageUp on the first tab should wrap to the last");
  Expect(SendKeyDown(shell, SDLK_PAGEDOWN, SDL_KMOD_CTRL),
         "Ctrl+PageDown at the last tab is handled");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 0,
         "Ctrl+PageDown on the last tab should wrap to the first");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tabswitch +2"),
         "tabswitch should accept a relative forward offset");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 2,
         "tabswitch +2 should move two tabs forward");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tabswitch -2"),
         "tabswitch should accept a relative backward offset");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 0,
         "tabswitch -2 should move two tabs back");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tabswitch 2"),
         "tabswitch should still accept an absolute one-based slot");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 1,
         "tabswitch 2 should activate the second tab");
}

void TestEditorTabMoveKeyChordsReorderAndClamp() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "tab move\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  for (int i = 0; i < 3; ++i) {
    const std::filesystem::path file = root / ("move_" + std::to_string(i) + ".txt");
    WriteFile(file, "alpha\n");
    WorkspaceShellTestAccess::OpenFile(shell, file);
  }
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) >= 3,
         "fixture should open at least three tabs");

  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  const std::filesystem::path first_path = WorkspaceShellTestAccess::OpenTabs(shell)[0].path;
  Expect(SendKeyDown(shell, SDLK_PAGEDOWN, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+PageDown should be consumed by the editor");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 1,
         "the moved tab stays active in its new slot");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell)[1].path == first_path,
         "Ctrl+Shift+PageDown moves the active tab one slot right");

  Expect(SendKeyDown(shell, SDLK_PAGEUP, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+PageUp should be consumed by the editor");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 0 &&
             WorkspaceShellTestAccess::OpenTabs(shell)[0].path == first_path,
         "Ctrl+Shift+PageUp moves it back");

  Expect(SendKeyDown(shell, SDLK_PAGEUP, SDL_KMOD_CTRL | SDL_KMOD_SHIFT),
         "Ctrl+Shift+PageUp at the first slot is still handled");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 0 &&
             WorkspaceShellTestAccess::OpenTabs(shell)[0].path == first_path,
         "moving left from the first slot clamps instead of wrapping to the end");

  WorkspaceShellTestAccess::OpenHelpAboutOverlay(shell);
  const auto help_rows = WorkspaceShellTestAccess::HelpAboutRows(shell);
  const auto detail_for = [&](std::string_view label) -> std::string {
    for (const microide::workspace::HelpAboutRow& row : help_rows) {
      if (row.label == label) {
        return row.detail;
      }
    }
    return {};
  };
  const std::string move_detail = detail_for("Move Tab");
  Expect(move_detail.find("Ctrl+Shift+PageDown") != std::string::npos &&
             move_detail.find("Ctrl+Shift+PageUp") != std::string::npos,
         "Help/About should list both Move Tab chords");
  const std::string switch_detail = detail_for("Switch Tab");
  Expect(switch_detail.find("Ctrl+PageDown") != std::string::npos &&
             switch_detail.find("Ctrl+PageUp") != std::string::npos,
         "Help/About should list both Switch Tab chords");
}

void TestEditorTabStripWheelStopsWhereTheOverflowButtonDoes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "wheel stop\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  for (int i = 0; i < 16; ++i) {
    const std::filesystem::path file =
        root / ("wheel_" + std::to_string(i) + "_long_name_for_overflow_test.txt");
    WriteFile(file, "alpha\n");
    WorkspaceShellTestAccess::OpenFile(shell, file);
  }

  constexpr std::size_t kTotalTabs = 16;
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  const auto at_left = WorkspaceShellTestAccess::EditorTabOverflowControls(shell);
  Expect(at_left.hidden_left == 0 && at_left.hidden_right > 0,
         "fixture should expose right-side editor-tab overflow");
  const std::size_t visible_when_full = kTotalTabs - at_left.hidden_right;
  Expect(visible_when_full > 1, "fixture should fit more than one tab at a time");

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const float wheel_x = layout.tab_strip.x + layout.tab_strip.w * 0.5f;
  const float wheel_y = layout.tab_strip.y + layout.tab_strip.h * 0.5f;
  for (int i = 0; i < 40; ++i) {
    SendMouseWheel(shell, wheel_x, wheel_y, -1);
  }

  const auto at_right = WorkspaceShellTestAccess::EditorTabOverflowControls(shell);
  Expect(at_right.hidden_right == 0, "wheeling right should reach the last tab");
  const std::size_t visible_at_right = kTotalTabs - at_right.hidden_left;
  Expect(visible_at_right > 1,
         "wheeling to the end must leave the strip full, not scroll past the last tab");
  Expect(visible_at_right + 1 >= visible_when_full,
         "the strip at the right end should hold about as many tabs as at the left end");

  for (int i = 0; i < 40; ++i) {
    SendMouseWheel(shell, wheel_x, wheel_y, 1);
  }
  Expect(WorkspaceShellTestAccess::EditorTabOverflowControls(shell).hidden_left == 0,
         "wheeling left should return the strip to the first tab");
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
  WorkspaceShellTestAccess::SetWindowSize(shell, 1600, 720);
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

void TestWorkspaceShellBreakpointGutterMenu() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetSettingValue(shell, "debug.enabled", "true");
  const std::filesystem::path path = "/tmp/project/main.cpp";

  WorkspaceShellTestAccess::OpenBreakpointContextMenu(shell, path, 9);
  Expect(!WorkspaceShellTestAccess::BreakpointContextMenuOpen(shell),
         "the gutter menu does not open on a line without a breakpoint");

  auto& store = WorkspaceShellTestAccess::BreakpointStore(shell);
  store.Toggle(path, 9);
  store.SetCondition(path, 9, "i > 5");
  store.SetHitCondition(path, 9, ">10");

  WorkspaceShellTestAccess::OpenBreakpointContextMenu(shell, path, 9);
  Expect(WorkspaceShellTestAccess::BreakpointContextMenuOpen(shell),
         "the gutter menu opens on an existing breakpoint");
  Expect(WorkspaceShellTestAccess::BreakpointContextMenuEnabledFlag(shell),
         "the menu seeds the enabled flag from an enabled breakpoint");

  Expect(WorkspaceShellTestAccess::ExecuteBreakpointMenuAction(
             shell, ActionId::DebugBreakpointToggleEnabled),
         "the toggle-enabled menu item executes");
  const auto* bps = store.FindByPath(path);
  Expect(bps != nullptr && bps->size() == 1 && !(*bps)[0].enabled,
         "toggle-enabled disables the breakpoint, keeping it in the store");

  WorkspaceShellTestAccess::OpenBreakpointContextMenu(shell, path, 9);
  Expect(!WorkspaceShellTestAccess::BreakpointContextMenuEnabledFlag(shell),
         "the menu seeds the enabled flag from a disabled breakpoint");

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

  const auto repaints_whole_window = [&](const auto& inv) {
    if (inv.full) {
      return true;
    }
    return std::any_of(inv.rects.begin(), inv.rects.end(), [&](const SDL_FRect& r) {
      return r.w >= kWindowWidth && r.h >= kWindowHeight;
    });
  };

  EnsureDummySdlVideoInitialized();
  SoftwareCanvas canvas(static_cast<int>(kWindowWidth), static_cast<int>(kWindowHeight));
  shell.Render(canvas.renderer(), static_cast<int>(kWindowWidth), static_cast<int>(kWindowHeight));
  const auto frame_damage = [&]() {
    return FrameDamage(shell, canvas, static_cast<int>(kWindowWidth), static_cast<int>(kWindowHeight),
                       shell.ConsumePendingRenderInvalidation());
  };
  (void)shell.ConsumePendingRenderInvalidation();
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "colorscheme light"),
         "the colorscheme command should run");
  Expect(repaints_whole_window(frame_damage()), "selecting a colorscheme must repaint the whole window");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "editor.colorscheme") ==
             std::optional<std::string>("light"),
         "the colorscheme command should activate the requested theme");

  (void)shell.ConsumePendingRenderInvalidation();
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "toggle-theme"),
         "the toggle-theme command should run");
  Expect(repaints_whole_window(frame_damage()), "toggling the theme must repaint the whole window");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "editor.colorscheme") !=
             std::optional<std::string>("light"),
         "toggle-theme should switch away from the light theme");

  const std::optional<std::string> before =
      WorkspaceShellTestAccess::GetSettingValue(shell, "editor.colorscheme");
  Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "colorscheme no-such-theme"),
         "an unknown colorscheme name must be rejected");
  Expect(WorkspaceShellTestAccess::GetSettingValue(shell, "editor.colorscheme") == before,
         "an unknown colorscheme name must leave the active theme alone");
}

void TestWorkspaceShellTabSwitchDefersLspHydration() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path left = root / "alpha.cpp";
  const std::filesystem::path right = root / "beta.cpp";
  WriteFile(left, "int alpha() { return 1; }\n");
  WriteFile(right, "int beta() { return 2; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, left);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, right);
  WorkspaceShellTestAccess::OnFramePresented(shell);
  Expect(!WorkspaceShellTestAccess::HasPendingLspBufferOpen(shell),
         "opening editor files should not leave LSP hydration pending");

  WorkspaceShellTestAccess::ActivateTab(shell, 1);
  Expect(WorkspaceShellTestAccess::HasPendingLspBufferOpen(shell),
         "activating a different tab must schedule (not synchronously run) LSP hydration");

  WorkspaceShellTestAccess::OnFramePresented(shell);
  Expect(!WorkspaceShellTestAccess::HasPendingLspBufferOpen(shell),
         "the post-present drain must consume the scheduled LSP hydration");
}

void TestWorkspaceShellSettingsKeepSelectionVisibleScrollsToRow() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "file.txt", "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 560);
  WorkspaceShellTestAccess::OpenSettingsOverlay(shell);
  WorkspaceShellTestAccess::SetSettingsOverlayCategory(shell, 0);

  const std::size_t rows =
      WorkspaceShellTestAccess::SettingsOverlayRowCountInSelectedCategory(shell);
  Expect(rows > 1, "the first settings category should have multiple rows");

  const int last_row = static_cast<int>(rows) - 1;
  WorkspaceShellTestAccess::SetSettingsOverlaySelectedRow(shell, last_row);
  WorkspaceShellTestAccess::SetSettingsOverlayScrollRow(shell, 0);
  WorkspaceShellTestAccess::EnsureSettingsSelectionVisible(shell);
  Expect(WorkspaceShellTestAccess::SettingsOverlayScrollRow(shell) > 0,
         "keeping the last row visible must scroll down from the top");
  Expect(WorkspaceShellTestAccess::SettingsOverlaySelectedRowFullyVisible(shell),
         "the selected last row must be fully within the settings pane after keep-visible");

  WorkspaceShellTestAccess::SetSettingsOverlaySelectedRow(shell, 0);
  WorkspaceShellTestAccess::EnsureSettingsSelectionVisible(shell);
  Expect(WorkspaceShellTestAccess::SettingsOverlayScrollRow(shell) == 0,
         "selecting the first row scrolls the settings pane to the top");
  Expect(WorkspaceShellTestAccess::SettingsOverlaySelectedRowFullyVisible(shell),
         "the first row is fully visible at the top of the pane");
}

void TestWorkspaceShellToggleDebuggerAgreesWithTheMenus() {
  WorkspaceShell shell;
  for (const char* off : {"no", "Off", "FALSE"}) {
    WorkspaceShellTestAccess::SetRawUserSetting(shell, "debug.enabled", off);
    Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::DebugToggleEnabled, {}),
           "Toggle Debugger runs");
    const auto value = WorkspaceShellTestAccess::GetSettingValue(shell, "debug.enabled");
    Expect(value.has_value() && *value == "true",
           std::string("a debugger stored as \"") + off + "\" is off, so the toggle turns it on");
  }
}

void TestWorkspaceShellSplitKeepsTheUntitledNumber() {
  TemporaryDirectory temp;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, temp.path());
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  for (int i = 0; i < 2; ++i) {
    Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::Tab, {}),
           "a new untitled tab opens");
  }
  auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  const std::string title = state.focused_group().active_tab().title;
  Expect(title == "Untitled-2", "the second untitled tab is numbered: " + title);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, microide::workspace::EditorSplitOrientation::Vertical),
         "the untitled tab splits");
  WorkspaceShellTestAccess::FocusEditorGroup(shell, 1);
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  const std::string split_title = state.editor_groups[1].active_tab().title;
  Expect(split_title == "Untitled-2",
         "the split shows the same buffer under the same name after activation: " + split_title);
}

void TestWorkspaceShellTabTitleChangeRepaintsTheStrip() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "project";
  WriteFile(root / "a.txt", "alpha\n");
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the project opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  for (int i = 0; i < 2; ++i) {
    Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::Tab, {}),
           "a new untitled tab opens");
  }
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Vertical),
         "the editor splits");
  WorkspaceShellTestAccess::FocusEditorGroup(shell, 0);
  microide::tests::SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  WorkspaceShellTestAccess::CurrentProjectState(shell).editor_groups[1].active_tab().title =
      "Untitled-99";
  workspace::RenderInvalidation unrelated;
  unrelated.rects.push_back(SDL_FRect{0.0f, kHeight - 8.0f, 8.0f, 8.0f});
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight, unrelated);
  Expect(diff.identical(), "the strip shows the new title: " + DescribePixelDiff(diff));
}

void TestWorkspaceShellSaveAsRenamesANamedTab() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "project";
  WriteFile(root / "README.md", "readme\n");
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the project opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, root / "README.md");
  for (const std::string target : {"-1", "notes/copy.md"}) {
    Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "save " + target),
           "save " + target + " writes the buffer");
    const auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
    const auto& tab = state.focused_group().active_tab();
    const std::filesystem::path buffer = WorkspaceShellTestAccess::ActiveEditor(shell).path();
    Expect(buffer.filename() == std::filesystem::path(target).filename(),
           "the buffer is named after the save: " + buffer.string());
    Expect(tab.path == buffer, "the tab's path follows the buffer: " + tab.path.string());
    Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, state.focused_group().active_tab_index) ==
               buffer.filename().string(),
           "the tab's label follows the buffer: " +
               WorkspaceShellTestAccess::TabDisplayTitle(shell, state.focused_group().active_tab_index));
  }

  WorkspaceShellTestAccess::OpenFile(shell, root / "README.md");
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Vertical),
         "the editor splits");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "save renamed.md"),
         "save renamed.md writes the shared buffer");
  const auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  for (std::size_t g = 0; g < state.editor_groups.size(); ++g) {
    const auto& tab = state.editor_groups[g].active_tab();
    Expect(tab.editor_state.has_value(), "each pane shows an editor");
    const std::filesystem::path buffer = tab.editor_state->viewport.path();
    Expect(tab.path == buffer && tab.title == buffer.filename().string(),
           "pane " + std::to_string(g) + "'s tab follows its buffer: tab " + tab.path.string() +
               " / " + tab.title + ", buffer " + buffer.string());
  }
}

void TestWorkspaceShellSaveAsRefusesAPathAnotherTabHolds() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "project";
  WriteFile(root / "README.md", "readme\n");
  std::filesystem::create_symlink("missing-target", root / "dangling");
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the project opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  for (const std::string target : {"dangling", "fresh.txt"}) {
    Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "open " + target),
           "open " + target + " gives it a buffer");
    WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("typed\n");
    WorkspaceShellTestAccess::OpenFile(shell, root / "README.md");
    Expect(!WorkspaceShellTestAccess::ExecuteCommandLine(shell, "save " + target),
           "save " + target + " is refused while another tab holds that file");
    Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == root / "README.md",
           "the refused save leaves the buffer named what it was: " +
               WorkspaceShellTestAccess::ActiveEditor(shell).path().string());
    const auto& groups = WorkspaceShellTestAccess::CurrentProjectState(shell).editor_groups;
    std::size_t buffers = 0;
    for (const auto& group : groups) {
      for (const auto& tab : group.open_tabs) {
        if (tab.editor_state.has_value() && tab.editor_state->viewport.path() == root / target) {
          ++buffers;
          Expect(tab.editor_state->viewport.dirty(), "the other tab keeps its unsaved text");
        }
      }
    }
    Expect(buffers == 1, target + " is one buffer, not " + std::to_string(buffers));
    std::error_code error;
    Expect(!std::filesystem::exists(root / target, error), "nothing was written to " + target);
  }
}

void TestWorkspaceShellSidebarShowUnderAMaximizedPanelShowsIt() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "project";
  WriteFile(root / "src" / "scheduler.cpp", "int x;\n");
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the project opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::TogglePanelMaximizedForTest(shell);
  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  (void)shell.ConsumePendingRenderInvalidation();
  Expect(WorkspaceShellTestAccess::CurrentProjectState(shell).panel.maximized, "the panel is maximized");

  (void)WorkspaceShellTestAccess::ExecuteCommandLine(shell, "sidebar-show src/scheduler.cpp");
  const workspace::RenderInvalidation redraw = shell.ConsumePendingRenderInvalidation();
  Expect(redraw.full || redraw.frame || !redraw.rects.empty(), "showing the sidebar asks for a frame");
  shell.Render(canvas.renderer(), 1280, 720);
  const auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  Expect(!state.panel.maximized, "the panel gives the sidebar its room back");
  Expect(state.surface.focus != microide::workspace::FocusTarget::Sidebar ||
             WorkspaceShellTestAccess::SidebarVisible(shell),
         "and the keyboard is on a sidebar that is shown");
}

void TestWorkspaceShellPaletteRowOverATabStripRepaintsOnHover() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "project";
  for (int i = 0; i < 12; ++i) {
    WriteFile(root / ("file_" + std::to_string(i) + ".txt"), "x\n");
  }
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the project opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenFile(shell, root / "file_0.txt");
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Horizontal),
         "the editor splits");
  for (int i = 1; i < 12; ++i) {
    WorkspaceShellTestAccess::OpenFileInNewTab(shell, root / ("file_" + std::to_string(i) + ".txt"));
  }
  WorkspaceShellTestAccess::OpenCommandPalette(shell);
  Expect(WorkspaceShellTestAccess::CommandPaletteOpen(shell), "the palette opens");
  SoftwareCanvas canvas(kWidth, kHeight);
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const SDL_FRect palette = WorkspaceShellTestAccess::ActiveOverlayRect(shell);
  const SDL_FRect strip = WorkspaceShellTestAccess::GroupTabStripRect(shell, 1);
  const float y = strip.y + strip.h / 2.0f;
  const float x = palette.x + palette.w / 2.0f;
  Expect(y > palette.y && y < palette.y + palette.h && strip.h > 0.0f,
         "the fixture puts the lower pane's tab strip under the palette");
  (void)DispatchEvents(shell, {MouseMotionEvent(5.0f, kHeight - 5.0f)});
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                DispatchEvents(shell, {MouseMotionEvent(x, y)}));
  Expect(diff.identical(), "the hovered palette row repaints: " + DescribePixelDiff(diff));
}

void TestWorkspaceShellBracketPairBrokenByAnEditRepaintsTheOpener() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "project";
  WriteFile(root / "main.cpp", "int main() {\n  return 0;\n}\n");
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the project opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::OpenFile(shell, root / "main.cpp");
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(2, 0);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();

  const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                DispatchEvents(shell, {TextInputEvent("//")}), pin_caret);
  Expect(editor.lines().LineView(2) == "//}",
         "the closer is commented out: " + std::string(editor.lines().LineView(2)));
  Expect(diff.identical(), "the opener's highlight goes with the pair: " + DescribePixelDiff(diff));
}

void TestWorkspaceShellSaveUnderAMaximizedPanelClearsTheTitleMark() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "project";
  WriteFile(root / "README.md", "readme\n");
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the project opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, root / "README.md");
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("edited ");
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::TogglePanelMaximizedForTest(shell);
  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  (void)shell.ConsumePendingRenderInvalidation();
  const std::string before = WorkspaceShellTestAccess::ControlUi(shell)["windowTitle"].AsString();
  Expect(before.starts_with("\xe2\x97\x8f"), "the unsaved buffer marks the title: " + before);

  Expect(WorkspaceShellTestAccess::SaveTab(
             shell, WorkspaceShellTestAccess::CurrentProjectState(shell).focused_group().active_tab_index),
         "the buffer saves");
  const workspace::RenderInvalidation redraw = shell.ConsumePendingRenderInvalidation();
  Expect(redraw.full || redraw.frame || !redraw.rects.empty(), "the save asks for a frame");
  shell.Render(canvas.renderer(), 1280, 720);
  const std::string after = WorkspaceShellTestAccess::ControlUi(shell)["windowTitle"].AsString();
  Expect(after.starts_with("README.md"), "the saved buffer's title has no unsaved mark: " + after);
}

void TestWorkspaceShellNewTabUnderAMaximizedPanelRetitlesTheWindow() {
  TemporaryDirectory temp;
  const std::filesystem::path root = temp.path() / "project";
  WriteFile(root / "a.txt", "alpha\n");
  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false), "the project opens");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, root / "a.txt");
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::TogglePanelMaximizedForTest(shell);
  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  (void)shell.ConsumePendingRenderInvalidation();
  Expect(WorkspaceShellTestAccess::CurrentProjectState(shell).panel.maximized, "the panel is maximized");

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tab"), "a new untitled tab opens");
  const workspace::RenderInvalidation redraw = shell.ConsumePendingRenderInvalidation();
  Expect(redraw.full || redraw.frame || !redraw.rects.empty(), "the activation asks for a frame");
  shell.Render(canvas.renderer(), 1280, 720);
  const std::string title = WorkspaceShellTestAccess::ControlUi(shell)["windowTitle"].AsString();
  Expect(title.starts_with("Untitled-1"), "the window title names the new tab: " + title);
}

}

void TestWorkspaceShellEditorClickHonorsHorizontalScroll() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "long.txt";
  WriteFile(source, std::string(2000, 'a') + "\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, source), "long file should open");

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(!editor.soft_wrap(), "word wrap should default off so the view scrolls horizontally");
  editor.MoveCursorTo(0, 1999, false);
  const auto metrics = WorkspaceShellTestAccess::ActiveEditorRenderMetrics(shell);
  const float char_width = WorkspaceShellTestAccess::TextCharWidth(shell);
  Expect(char_width > 0.0f, "char width should be positive");
  const std::size_t horizontal_scroll = editor.horizontal_scroll();
  Expect(horizontal_scroll > 0,
         "moving the caret to the far right should scroll the view horizontally");

  const std::size_t screen_cells = 5;
  const float click_x = metrics.text_x + (static_cast<float>(screen_cells) + 0.25f) * char_width;
  const float click_y = metrics.first_line_y + metrics.line_height * 0.5f;
  Expect(SendMouseDown(shell, click_x, click_y, SDL_BUTTON_LEFT, 1),
         "the editor click should be handled");

  Expect(editor.cursor_line() == 0, "the click should stay on the only line");
  Expect(editor.cursor_column() == horizontal_scroll + screen_cells,
         "click caret column must honor the horizontal scroll exactly once (no double-count)");
}

void TestWorkspaceShellEditorClickOnWrappedContinuationRow() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "wrapped.txt";
  WriteFile(source, std::string(2000, 'a') + "\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, source), "long file should open");
  WorkspaceShellTestAccess::SetSettingValue(shell, "editor.wrap", "word");

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(editor.soft_wrap(), "editor.wrap=word should reach the active editor");
  const auto metrics = WorkspaceShellTestAccess::ActiveEditorRenderMetrics(shell);
  const float char_width = WorkspaceShellTestAccess::TextCharWidth(shell);
  Expect(char_width > 0.0f, "char width should be positive");
  Expect(editor.visual_line_count() > 2, "the long line should wrap into several rows");
  Expect(editor.scroll_line() == 0, "the view starts at the top");

  const auto row = editor.WrappedVisualRowLayout(1);
  Expect(row.visual_start > 0, "row 1 is a continuation row of the same logical line");
  const std::size_t screen_cells = 5;
  const float click_x = metrics.text_x + (static_cast<float>(screen_cells) + 0.25f) * char_width;
  const float click_y = metrics.first_line_y + metrics.line_height * 1.5f;

  Expect(SendMouseDown(shell, click_x, click_y, SDL_BUTTON_LEFT, 1),
         "the editor click should be handled");
  Expect(editor.cursor_line() == 0, "the click stays on the only logical line");
  Expect(editor.cursor_column() == row.visual_start + screen_cells,
         "a click on a continuation row lands under the pointer, not near the line start");

  editor.MoveCursorTo(0, 0, false);
  Expect(SendMouseDown(shell, click_x, click_y, SDL_BUTTON_RIGHT, 1),
         "the editor right-click should be handled");
  Expect(editor.cursor_line() == 0 && editor.cursor_column() == row.visual_start + screen_cells,
         "right-click retargeting resolves the continuation row the same way");
}


void TestWorkspaceShellUnfocusedSplitPaneTabIsDimmed() {
  using microide::workspace::EditorSplitOrientation;
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.txt";
  WriteFile(alpha, "alpha\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the dimming fixture needs a project tab so the editor strip paints");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, alpha);
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(shell, EditorSplitOrientation::Vertical),
         "the dimming fixture needs two panes");
  const std::size_t focused = WorkspaceShellTestAccess::FocusedGroupIndex(shell);
  const std::size_t unfocused = focused == 0 ? 1 : 0;

  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);

  const auto accent_pixel = [&](std::size_t group_index) {
    const std::size_t active = WorkspaceShellTestAccess::GroupActiveTabIndex(shell, group_index);
    const SDL_FRect rect = WorkspaceShellTestAccess::GroupEditorTabRect(shell, group_index, active);
    Expect(rect.w > 0.0f, "both panes must show their active tab for this comparison");
    return canvas.PixelAt(static_cast<int>(rect.x + rect.w * 0.5f),
                          static_cast<int>(rect.y) + 1);
  };
  const SDL_Color focused_accent = accent_pixel(focused);
  const SDL_Color unfocused_accent = accent_pixel(unfocused);
  Expect(!(unfocused_accent.r == focused_accent.r && unfocused_accent.g == focused_accent.g &&
           unfocused_accent.b == focused_accent.b),
         "the unfocused pane's active tab must not paint the same accent as the focused one; both " +
             std::to_string(focused_accent.r) + "," + std::to_string(focused_accent.g) + "," +
             std::to_string(focused_accent.b));
  const auto luminance = [](SDL_Color c) {
    return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
  };
  Expect(luminance(unfocused_accent) < luminance(focused_accent),
         "the unfocused pane's accent is the dimmer of the two");
  const SDL_FRect strip = WorkspaceShellTestAccess::GroupTabStripRect(shell, unfocused);
  const SDL_Color strip_background =
      canvas.PixelAt(static_cast<int>(strip.x + strip.w) - 3, static_cast<int>(strip.y) + 1);
  Expect(!(unfocused_accent.r == strip_background.r && unfocused_accent.g == strip_background.g &&
           unfocused_accent.b == strip_background.b),
         "the unfocused pane's accent must stay visible against its strip");
}


void TestWorkspaceShellSameNameTabsShowTheirParentFolder() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path a = root / "a" / "index.txt";
  const std::filesystem::path b = root / "b" / "index.txt";
  const std::filesystem::path other = root / "other.txt";
  WriteFile(a, "a\n");
  WriteFile(b, "b\n");
  WriteFile(other, "o\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, a);
  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 0) == "index.txt",
         "a lone tab shows just its file name");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, b), "the second index.txt opens");
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, other), "other.txt opens");
  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 0) == "index.txt \xe2\x80\x94 a",
         "the first same-name tab shows its parent folder: [" +
             WorkspaceShellTestAccess::TabDisplayTitle(shell, 0) + "]");
  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 1) == "index.txt \xe2\x80\x94 b",
         "the second same-name tab shows its parent folder");
  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 2) == "other.txt",
         "a uniquely named tab stays bare");
}

void TestWorkspaceShellAnEditThatChangesTheLanguageRepaintsThePane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path hook = root / "post-update";
  WriteFile(hook, "#!/bin/sh\n#\n# An example hook\n#\nexec git update-server-info\n");
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, hook);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWidth, kHeight);
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).language_id() == "shell",
         "a shell script by its shebang: " + WorkspaceShellTestAccess::ActiveEditor(shell).language_id());
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);
  const auto pin_caret = [&]() { WorkspaceShellTestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); };
  SoftwareCanvas canvas(kWidth, kHeight);
  pin_caret();
  shell.Render(canvas.renderer(), kWidth, kHeight);
  (void)shell.ConsumePendingRenderInvalidation();
  const auto typed = PaintAndDiffWithFullRepaint(shell, canvas, kWidth, kHeight,
                                                 DispatchEvents(shell, {TextInputEvent(" ")}), pin_caret);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).language_id() != "shell",
         "the shebang no longer starts the file: no longer a shell script");
  Expect(typed.identical(), "every row repaints in the new language; " + DescribePixelDiff(typed));
}

void RegisterWorkspaceShellChromeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/TreeContextMenuHoverMatchesTheRowBuilder",
          TestWorkspaceShellTreeContextMenuHoverMatchesTheRowBuilder);
  AddTest(tests, "WorkspaceShell/SameNameTabsShowTheirParentFolder",
          TestWorkspaceShellSameNameTabsShowTheirParentFolder);
  AddTest(tests, "WorkspaceShell/TabSwitchDefersLspHydration",
          TestWorkspaceShellTabSwitchDefersLspHydration);
  AddTest(tests, "WorkspaceShell/SettingsKeepSelectionVisibleScrollsToRow",
          TestWorkspaceShellSettingsKeepSelectionVisibleScrollsToRow);
  AddTest(tests, "WorkspaceShell/EditorClickHonorsHorizontalScroll",
          TestWorkspaceShellEditorClickHonorsHorizontalScroll);
  AddTest(tests, "WorkspaceShell/EditorClickOnWrappedContinuationRow",
          TestWorkspaceShellEditorClickOnWrappedContinuationRow);
  AddTest(tests, "WorkspaceShell/ColorschemeChangeRequestsRepaint",
          TestColorschemeChangeRequestsRepaint);
  AddTest(tests, "WorkspaceShell/BreakpointGutterMenu", TestWorkspaceShellBreakpointGutterMenu);
  AddTest(tests, "WorkspaceShell/ViewMenuToggleReflectsBackingSetting",
          TestViewMenuToggleReflectsBackingSetting);
  AddTest(tests, "WorkspaceShell/EditorTabStripOverflowControlsScrollAndCount",
          TestEditorTabStripOverflowControlsScrollAndCount);
  AddTest(tests, "WorkspaceShell/EditorTabStripUsesLeftGapWhenOnlyRightOverflowRemains",
          TestEditorTabStripUsesLeftGapWhenOnlyRightOverflowRemains);
  AddTest(tests, "WorkspaceShell/EditorWheelScrollsHorizontally",
          TestEditorWheelScrollsHorizontally);
  AddTest(tests, "WorkspaceShell/EditorTabSwitchAcceptsRelativeOffsetsAndKeyChords",
          TestEditorTabSwitchAcceptsRelativeOffsetsAndKeyChords);
  AddTest(tests, "WorkspaceShell/EditorTabMoveKeyChordsReorderAndClamp",
          TestEditorTabMoveKeyChordsReorderAndClamp);
  AddTest(tests, "WorkspaceShell/EditorTabStripWheelStopsWhereTheOverflowButtonDoes",
          TestEditorTabStripWheelStopsWhereTheOverflowButtonDoes);
  AddTest(tests, "WorkspaceShell/EditorTabOverflowButtonExpandsForDoubleDigitHiddenCount",
          TestEditorTabOverflowButtonExpandsForDoubleDigitHiddenCount);
  AddTest(tests, "WorkspaceShell/ClosingTabsWhileScrolledRecomputesOverflowImmediately",
          TestClosingTabsWhileScrolledRecomputesOverflowImmediately);
  AddTest(tests, "WorkspaceShell/MenuBarOmitsRemovedMenus",
          TestWorkspaceShellMenuBarOmitsRemovedMenus);
  AddTest(tests, "WorkspaceShell/MenuBarLayoutIsAllocationFree",
          TestWorkspaceShellMenuBarLayoutIsAllocationFree);
  AddTest(tests, "WorkspaceShell/MenuBarShowsChevronWhenTruncated",
          TestWorkspaceShellMenuBarShowsChevronWhenTruncated);
  AddTest(tests, "WorkspaceShell/CompactMenuOverflowButtonIsInteractive",
          TestWorkspaceShellCompactMenuOverflowButtonIsInteractive);
  AddTest(tests, "WorkspaceShell/MenuOverflowPopupFollowsTheWindow", TestWorkspaceShellMenuOverflowPopupFollowsTheWindow);
  AddTest(tests, "WorkspaceShell/CompactMenuOverflowPaintsWhatAFullRepaintPaints",
          TestWorkspaceShellCompactMenuOverflowPaintsWhatAFullRepaintPaints);
  AddTest(tests, "WorkspaceShell/CompactMenuOverflowRowsOpenAnchoredMenus",
          TestWorkspaceShellCompactMenuOverflowRowsOpenAnchoredMenus);
  AddTest(tests, "WorkspaceShell/GitSidebarHeaderButtonsHaveTooltips",
          TestWorkspaceShellGitSidebarHeaderButtonsHaveTooltips);
  AddTest(tests, "WorkspaceShell/CompactMenuOverflowIsKeyboardNavigable",
          TestWorkspaceShellCompactMenuOverflowIsKeyboardNavigable);
  AddTest(tests, "WorkspaceShell/CompactMenuOverflowEscapeClosesIt",
          TestWorkspaceShellCompactMenuOverflowEscapeClosesIt);
  AddTest(tests, "WorkspaceShell/CompactMenuOverflowMotionTracksHoveredRow",
          TestWorkspaceShellCompactMenuOverflowMotionTracksHoveredRow);
  AddTest(tests, "WorkspaceShell/MenuBarHoverSwitchesActiveMenu",
          TestWorkspaceShellMenuBarHoverSwitchesActiveMenu);
  AddTest(tests, "WorkspaceShell/MenuEventsOweAFrame",
          TestWorkspaceShellMenuEventsOweAFrame);
  AddTest(tests, "WorkspaceShell/PopupRowHoverOwesAFrame",
          TestWorkspaceShellPopupRowHoverOwesAFrame);
  AddTest(tests, "WorkspaceShell/TreeSidebarHeaderHoverReturnsButtonOnlyInvalidation",
          TestWorkspaceShellTreeSidebarHeaderHoverReturnsButtonOnlyInvalidation);
  AddTest(tests, "WorkspaceShell/SearchSidebarHeaderHoverReturnsButtonOnlyInvalidation",
          TestWorkspaceShellSearchSidebarHeaderHoverReturnsButtonOnlyInvalidation);
  AddTest(tests, "WorkspaceShell/GitSidebarHeaderHoverReturnsButtonOnlyInvalidation",
          TestWorkspaceShellGitSidebarHeaderHoverReturnsButtonOnlyInvalidation);
  AddTest(tests, "WorkspaceShell/EditorLineRangeDirtyRectCoversItsLines",
          TestWorkspaceShellEditorLineRangeDirtyRectCoversItsLines);
  AddTest(tests, "WorkspaceShell/FindWidgetHoverRepaintsTheCard",
          TestWorkspaceShellFindWidgetHoverRepaintsTheCard);
  AddTest(tests, "WorkspaceShell/StatusBarSegmentsExplainThemselvesOnHover",
          TestWorkspaceShellStatusBarSegmentsExplainThemselvesOnHover);
  AddTest(tests, "WorkspaceShell/FindWidgetControlsNameThemselvesOnHover",
          TestWorkspaceShellFindWidgetControlsNameThemselvesOnHover);
  AddTest(tests, "WorkspaceShell/TooltipCardIsAnchoredToItsControl",
          TestWorkspaceShellTooltipCardIsAnchoredToItsControl);
  AddTest(tests, "WorkspaceShell/OpenMenuSuppressesUnderlyingTabTooltip",
          TestWorkspaceShellOpenMenuSuppressesUnderlyingTabTooltip);
  AddTest(tests, "WorkspaceShell/OverflowPopupSuppressesUnderlyingTabTooltip",
          TestWorkspaceShellOverflowPopupSuppressesUnderlyingTabTooltip);
  AddTest(tests, "WorkspaceShell/StatusRowShowsLspReadinessAndInFlightState",
          TestWorkspaceShellStatusRowShowsLspReadinessAndInFlightState);
  AddTest(tests, "WorkspaceShell/EditorCaretDirtyRectFollowsActiveCaret",
          TestWorkspaceShellEditorCaretDirtyRectFollowsActiveCaret);
  AddTest(tests, "WorkspaceShell/AnEditThatChangesTheLanguageRepaintsThePane",
          TestWorkspaceShellAnEditThatChangesTheLanguageRepaintsThePane);
  AddTest(tests, "WorkspaceShell/EditorTypingReturnsPartialEditorInvalidation",
          TestWorkspaceShellEditorTypingReturnsPartialEditorInvalidation);
  AddTest(tests, "WorkspaceShell/ActionWithNoTreeMenuOpenLeavesTheChromeAlone",
          TestWorkspaceShellActionWithNoTreeMenuOpenLeavesTheChromeAlone);
  AddTest(tests, "WorkspaceShell/WrappedEditorLineRangeRectCoversContinuationRows",
          TestWorkspaceShellWrappedEditorLineRangeRectCoversContinuationRows);
  AddTest(tests, "WorkspaceShell/CommandTextInputReturnsPartialCommandInvalidation",
          TestWorkspaceShellCommandTextInputReturnsPartialCommandInvalidation);
  AddTest(tests, "WorkspaceShell/CommandPasteShortcutUsesSharedTextInputPath",
          TestWorkspaceShellCommandPasteShortcutUsesSharedTextInputPath);
  AddTest(tests, "WorkspaceShell/CommandPaletteRanksExactCommandNameFirst",
          TestWorkspaceShellCommandPaletteRanksExactCommandNameFirst);
  AddTest(tests, "WorkspaceShell/CommandPaletteMatchesQueryWordsInAnyOrder",
          TestWorkspaceShellCommandPaletteMatchesQueryWordsInAnyOrder);
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
  AddTest(tests, "WorkspaceShell/OpenedPaletteShowsItsCaretOnTheFirstFrame",
          TestWorkspaceShellOpenedPaletteShowsItsCaretOnTheFirstFrame);
  AddTest(tests, "WorkspaceShell/UnfocusedEditorDoesNotPaintTheBlinkingCaret",
          TestWorkspaceShellUnfocusedEditorDoesNotPaintTheBlinkingCaret);
  AddTest(tests, "WorkspaceShell/CaretBlinkIsDecidedOncePerFrame",
          TestWorkspaceShellCaretBlinkIsDecidedOncePerFrame);
  AddTest(tests, "WorkspaceShell/EditorRepaintUnderAnOpenMenuKeepsTheEditor",
          TestWorkspaceShellEditorRepaintUnderAnOpenMenuKeepsTheEditor);
  AddTest(tests, "WorkspaceShell/AStatusSegmentModalOwnsThePointer",
          TestWorkspaceShellAStatusSegmentModalOwnsThePointer);
  AddTest(tests, "WorkspaceShell/TreeFollowingTheActiveTabRepaints",
          TestWorkspaceShellTreeFollowingTheActiveTabRepaints);
  AddTest(tests, "WorkspaceShell/TerminalFindBarOwnsTheBlink",
          TestWorkspaceShellTerminalFindBarOwnsTheBlink);
  AddTest(tests, "WorkspaceShell/NarrowGitViewKeepsItsLabelsInside",
          TestWorkspaceShellNarrowGitViewKeepsItsLabelsInside);
  AddTest(tests, "WorkspaceShell/TabTooltipFollowsATabMovedUnderThePointer",
          TestWorkspaceShellTabTooltipFollowsATabMovedUnderThePointer);
  AddTest(tests, "WorkspaceShell/AStackedModalRedimsTheWindow",
          TestWorkspaceShellAStackedModalRedimsTheWindow);
  AddTest(tests, "WorkspaceShell/PanelDividerResetRepaintsTheLayout",
          TestWorkspaceShellPanelDividerResetRepaintsTheLayout);
  AddTest(tests, "WorkspaceShell/SidebarDividerDragPaintsCleanly",
          TestWorkspaceShellSidebarDividerDragPaintsCleanly);
  AddTest(tests, "WorkspaceShell/EditorSplitDividerDragPaintsCleanly",
          TestWorkspaceShellEditorSplitDividerDragPaintsCleanly);
  AddTest(tests, "WorkspaceShell/PaneTabStripRepaintKeepsTheSplitDivider",
          TestWorkspaceShellPaneTabStripRepaintKeepsTheSplitDivider);
  AddTest(tests, "WorkspaceShell/SaveAsRepaintsTheTabTitle",
          TestWorkspaceShellSaveAsRepaintsTheTabTitle);
  AddTest(tests, "WorkspaceShell/APromptOverSettingsOwnsThePointer",
          TestWorkspaceShellAPromptOverSettingsOwnsThePointer);
  AddTest(tests, "WorkspaceShell/SettingsFilterTextStaysInItsBar",
          TestWorkspaceShellSettingsFilterTextStaysInItsBar);
  AddTest(tests, "WorkspaceShell/ReplacingTheFindWidgetRepaintsItsMatches",
          TestWorkspaceShellReplacingTheFindWidgetRepaintsItsMatches);
  AddTest(tests, "WorkspaceShell/SidebarShowRepaintsTheMovedEditor",
          TestWorkspaceShellSidebarShowRepaintsTheMovedEditor);
  AddTest(tests, "WorkspaceShell/ASaveRepaintsTheProjectTabsDirtyMarker",
          TestWorkspaceShellASaveRepaintsTheProjectTabsDirtyMarker);
  AddTest(tests, "WorkspaceShell/ASplitDividerThatMovedRepaintsOnAnUnrelatedFrame",
          TestWorkspaceShellASplitDividerThatMovedRepaintsOnAnUnrelatedFrame);
  AddTest(tests, "WorkspaceShell/ATreeThatMovedRepaintsOnAnUnrelatedFrame",
          TestWorkspaceShellATreeThatMovedRepaintsOnAnUnrelatedFrame);
  AddTest(tests, "WorkspaceShell/ASaveRepaintsTheDirtyMarker",
          TestWorkspaceShellASaveRepaintsTheDirtyMarker);
  AddTest(tests, "WorkspaceShell/AnEditorRepaintKeepsTheDebugPaneDivider",
          TestWorkspaceShellAnEditorRepaintKeepsTheDebugPaneDivider);
  AddTest(tests, "WorkspaceShell/ClosingSettingsOverTheFindWidgetRepaintsItsArea",
          TestWorkspaceShellClosingSettingsOverTheFindWidgetRepaintsItsArea);
  AddTest(tests, "WorkspaceShell/DroppingAPanelTabClearsItsGhost",
          TestWorkspaceShellDroppingAPanelTabClearsItsGhost);
  AddTest(tests, "WorkspaceShell/PanelHeaderButtonRepaintsItsHover",
          TestWorkspaceShellPanelHeaderButtonRepaintsItsHover);
  AddTest(tests, "WorkspaceShell/RemoteProjectTabShowsHostAndConnectionCorner",
          TestWorkspaceShellRemoteProjectTabShowsHostAndConnectionCorner);
  AddTest(tests, "WorkspaceShell/EmptyTerminalPanelShowsItsNewTerminalButton",
          TestWorkspaceShellEmptyTerminalPanelShowsItsNewTerminalButton);
  AddTest(tests, "WorkspaceShell/MenuArrowsWalkOnlyTheMenuBar",
          TestWorkspaceShellMenuArrowsWalkOnlyTheMenuBar);
  AddTest(tests, "WorkspaceShell/AModalReleasesAHoveredWindowControl",
          TestWorkspaceShellAModalReleasesAHoveredWindowControl);
  AddTest(tests, "WorkspaceShell/ClosingAnAnchoredMenuReleasesItsButton",
          TestWorkspaceShellClosingAnAnchoredMenuReleasesItsButton);
  AddTest(tests, "Breadcrumbs/FolderAtNamesTheSegmentUnderTheClick", TestBreadcrumbFolderAtNamesTheSegmentUnderTheClick);
  AddTest(tests, "WorkspaceShell/MenuBarFitsOrOverflowsNeverTruncates", TestWorkspaceShellMenuBarFitsOrOverflowsNeverTruncates);
  AddTest(tests, "WorkspaceShell/ChromeSegmentsAreInteractive", TestWorkspaceShellChromeSegmentsAreInteractive);
  AddTest(tests, "WorkspaceShell/SettingsOverAFocusedTerminalHidesItsCaret",
          TestWorkspaceShellSettingsOverAFocusedTerminalHidesItsCaret);
  AddTest(tests, "WorkspaceShell/MaximizedPanelPaintsNoHiddenSurface",
          TestWorkspaceShellMaximizedPanelPaintsNoHiddenSurface);
  AddTest(tests, "WorkspaceShell/TerminalIsFixedForAPreparedFrame",
          TestWorkspaceShellTerminalIsFixedForAPreparedFrame);
  AddTest(tests, "WorkspaceShell/BlinkInALowerPaneKeepsTheDivider",
          TestWorkspaceShellBlinkInALowerPaneKeepsTheDivider);
  AddTest(tests, "WorkspaceShell/EditRepaintsEveryPaneOfTheDocument",
          TestWorkspaceShellEditRepaintsEveryPaneOfTheDocument);
  AddTest(tests, "WorkspaceShell/MenuRowOverTheSidebarViewButtonsRunsTheRow",
          TestWorkspaceShellMenuRowOverTheSidebarViewButtonsRunsTheRow);
  AddTest(tests, "WorkspaceShell/MenuTakingTheKeyboardRepaintsTheFieldFocusRing",
          TestWorkspaceShellMenuTakingTheKeyboardRepaintsTheFieldFocusRing);
  AddTest(tests, "WorkspaceShell/OpeningAMenuTakesTheCaretOffScreen",
          TestWorkspaceShellOpeningAMenuTakesTheCaretOffScreen);
  AddTest(tests, "WorkspaceShell/RowUnderAnOpenMenuDoesNotHoverThroughIt",
          TestWorkspaceShellRowUnderAnOpenMenuDoesNotHoverThroughIt);
  AddTest(tests, "WorkspaceShell/EscapeFromFindInFilesRepaintsTheField",
          TestWorkspaceShellEscapeFromFindInFilesRepaintsTheField);
  AddTest(tests, "WorkspaceShell/HoveringIntoAClickedMenuRepaintsItsFirstRow",
          TestWorkspaceShellHoveringIntoAClickedMenuRepaintsItsFirstRow);
  AddTest(tests, "WorkspaceShell/OpeningAMenuByClickKeepsTheSidebar",
          TestWorkspaceShellOpeningAMenuByClickKeepsTheSidebar);
  AddTest(tests, "WorkspaceShell/SecondarySidebarsDoNotPaintTheFileTree",
          TestWorkspaceShellSecondarySidebarsDoNotPaintTheFileTree);
  AddTest(tests, "WorkspaceShell/CallHierarchyIsGatedLikeItsSiblings",
          TestWorkspaceShellCallHierarchyIsGatedLikeItsSiblings);
  AddTest(tests, "WorkspaceShell/SettingsRowsLiftOnHover",
          TestWorkspaceShellSettingsRowsLiftOnHover);
  AddTest(tests, "WorkspaceShell/PluginSurfacePanelDrawsItsFocusRing",
          TestWorkspaceShellPluginSurfacePanelDrawsItsFocusRing);
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
  AddTest(tests, "WorkspaceShell/RevealInFileExplorerValidatesPathSynchronously",
          TestWorkspaceShellRevealInFileExplorerValidatesPathSynchronously);
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
  AddTest(tests, "WorkspaceShell/LayoutMemoTracksEveryInput",
          TestWorkspaceShellLayoutMemoTracksEveryInput);
  AddTest(tests, "WorkspaceShell/EditorGroupRectsMemoFollowsTheTree",
          TestWorkspaceShellEditorGroupRectsMemoFollowsTheTree);
  AddTest(tests, "WorkspaceShell/RenderBuildsEditorViewModelOncePerSimplePaneFrame",
          TestWorkspaceShellRenderBuildsEditorViewModelOncePerSimplePaneFrame);
  AddTest(tests, "WorkspaceShell/FoldingRefreshRunsOncePerPreparedFrame",
          TestWorkspaceShellFoldingRefreshRunsOncePerPreparedFrame);
  AddTest(tests, "WorkspaceShell/FramePrepCountersRunOncePerPreparedFrame",
          TestWorkspaceShellFramePrepCountersRunOncePerPreparedFrame);
#endif
  AddTest(tests, "WorkspaceShell/FileCloseAllTabsClosesOpenEditorTabs",
          TestWorkspaceShellFileCloseAllTabsClosesOpenEditorTabs);
  AddTest(tests, "WorkspaceShell/TitleBarDragRegionDoesNotFabricateMaximizeToggle",
          TestWorkspaceShellTitleBarDragRegionDoesNotFabricateMaximizeToggle);
  AddTest(tests, "WorkspaceShell/FullscreenStateDisablesResizableFrameHitTest",
          TestWorkspaceShellFullscreenStateDisablesResizableFrameHitTest);
  AddTest(tests, "WorkspaceShell/WindowPresentationStateUpdatesChromeAndSize",
          TestWorkspaceShellWindowPresentationStateUpdatesChromeAndSize);
  AddTest(tests, "WorkspaceShell/RenderedFrameIsFullyOpaque",
          TestWorkspaceShellRenderedFrameIsFullyOpaque);
  AddTest(tests, "WorkspaceShell/StatusBarColumnIsTheVisibleColumn",
          TestWorkspaceShellStatusBarColumnIsTheVisibleColumn);
  AddTest(tests, "WorkspaceShell/StatusBarHidesEditorSegmentsWithNoEditorOpen",
          TestWorkspaceShellStatusBarHidesEditorSegmentsWithNoEditorOpen);
  AddTest(tests, "WorkspaceShell/StatusBarRepaintsWhenItsValuesChange",
          TestWorkspaceShellStatusBarRepaintsWhenItsValuesChange);
  AddTest(tests, "WorkspaceShell/SettingsAndHelpDimTheEditorBehindThem",
          TestWorkspaceShellSettingsAndHelpDimTheEditorBehindThem);
  AddTest(tests, "WorkspaceShell/SaveAsRenamesANamedTab", TestWorkspaceShellSaveAsRenamesANamedTab);
  AddTest(tests, "WorkspaceShell/SaveAsRefusesAPathAnotherTabHolds",
          TestWorkspaceShellSaveAsRefusesAPathAnotherTabHolds);
  AddTest(tests, "WorkspaceShell/SaveUnderAMaximizedPanelClearsTheTitleMark",
          TestWorkspaceShellSaveUnderAMaximizedPanelClearsTheTitleMark);
  AddTest(tests, "WorkspaceShell/NewTabUnderAMaximizedPanelRetitlesTheWindow",
          TestWorkspaceShellNewTabUnderAMaximizedPanelRetitlesTheWindow);
  AddTest(tests, "WorkspaceShell/BracketPairBrokenByAnEditRepaintsTheOpener",
          TestWorkspaceShellBracketPairBrokenByAnEditRepaintsTheOpener);
  AddTest(tests, "WorkspaceShell/PaletteRowOverATabStripRepaintsOnHover",
          TestWorkspaceShellPaletteRowOverATabStripRepaintsOnHover);
  AddTest(tests, "WorkspaceShell/SidebarShowUnderAMaximizedPanelShowsIt",
          TestWorkspaceShellSidebarShowUnderAMaximizedPanelShowsIt);
  AddTest(tests, "WorkspaceShell/SplitKeepsTheUntitledNumber", TestWorkspaceShellSplitKeepsTheUntitledNumber);
  AddTest(tests, "WorkspaceShell/TabTitleChangeRepaintsTheStrip", TestWorkspaceShellTabTitleChangeRepaintsTheStrip);
  AddTest(tests, "WorkspaceShell/ToggleDebuggerAgreesWithTheMenus",
          TestWorkspaceShellToggleDebuggerAgreesWithTheMenus);
  AddTest(tests, "WorkspaceShell/UnfocusedSplitPaneTabIsDimmed",
          TestWorkspaceShellUnfocusedSplitPaneTabIsDimmed);
}

}
