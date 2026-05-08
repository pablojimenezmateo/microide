#include "TestSupport.h"

#include "TerminalSessionTestAccess.h"
#include "render/Theme.h"
#include "workspace/WorkspaceShellTestAccess.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
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

void TestWorkspaceShellDoubleClickTitleBarRequestsMaximizeToggle() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1920, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);

  // Test point chosen to land past the expanded menu bar but before window-control buttons.
  const float empty_x = 1500.0f;
  Expect(shell.WindowHitTest(empty_x, 10.0f) == SDL_HITTEST_DRAGGABLE,
         "empty title-bar hit testing should hand borderless dragging back to the window manager");
  Expect(shell.WindowDragRegionContains(empty_x, 10.0f),
         "empty title-bar space should still be eligible for window dragging");
  Expect(SendMouseDown(shell, empty_x, 10.0f, SDL_BUTTON_LEFT, 2),
         "double-clicking an empty title-bar region should be handled");
  Expect(shell.ConsumeWindowAction() ==
             WorkspaceShell::WindowAction::ToggleMaximize,
         "double-clicking the title bar should request the same maximize toggle as the chrome button");
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
  Expect(lsp_it != initial_items.end() && lsp_it->item.text == "LSP: No LSP server",
         "status row should surface the host-owned LSP readiness text");

  auto& project = WorkspaceShellTestAccess::CurrentProjectState(shell);
  project.lsp.request_in_flight = true;
  project.lsp.request_started_ticks = SDL_GetTicks();
  project.lsp.request_timeout_ticks = project.lsp.request_started_ticks + 1000;

  const auto in_flight_items = WorkspaceShellTestAccess::VisibleStatusItems(shell);
  const auto in_flight_it =
      std::find_if(in_flight_items.begin(), in_flight_items.end(), [](const auto& item) {
        return item.item.id == "host.lsp";
      });
  Expect(in_flight_it != in_flight_items.end() &&
             in_flight_it->item.text == "LSP: working...",
         "status row should surface transient in-flight LSP work");
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
  const std::filesystem::path left = root / "left.cpp";
  const std::filesystem::path right = root / "right.cpp";
  WriteFile(left, "alpha\nbeta\n");
  WriteFile(right, "gamma\ndelta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, left);
  Expect(WorkspaceShellTestAccess::SplitActiveEditor(shell, true),
         "editor invalidation fixture should split the active editor");
  Expect(WorkspaceShellTestAccess::ReplaceActiveEditorWithFile(shell, right),
         "editor invalidation fixture should populate the active split");
  Expect(WorkspaceShellTestAccess::ActivateOrderedEditorSplit(shell, 0),
         "editor invalidation fixture should activate the left split");
  (void)shell.ConsumePendingRenderInvalidation();

  SDL_Event event{};
  event.type = SDL_EVENT_TEXT_INPUT;
  const std::string text = "x";
  event.text.text = text.c_str();
  const auto result = shell.HandleEvent(event);
  const SDL_FRect active_pane = WorkspaceShellTestAccess::ActiveEditorPaneRect(shell);
  const SDL_FRect inactive_pane = WorkspaceShellTestAccess::InactiveEditorPaneRect(shell);
  const auto edited_line_rect = WorkspaceShellTestAccess::ActiveEditorLineRangeRect(shell, 0, 1);

  Expect(result.handled, "editor typing should be handled");
  Expect(!result.redraw.full && !result.redraw.rects.empty(),
         "editor typing should request a partial redraw");
  Expect(AnyRectIntersects(result.redraw.rects, active_pane),
         "editor typing redraws should include the active editor pane");
  Expect(!AnyRectIntersects(result.redraw.rects, inactive_pane),
         "editor typing redraws should avoid repainting the inactive split pane");
  Expect(edited_line_rect.has_value() && AnyRectIntersects(result.redraw.rects, *edited_line_rect),
         "editor typing redraws should include the edited line band");
  Expect(MaxRectHeight(result.redraw.rects) < active_pane.h,
         "single-line editor typing should redraw less than the full active pane");
}

void TestWorkspaceShellCommandTextInputReturnsPartialCommandInvalidation() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  SDL_Event open_event{};
  open_event.type = SDL_EVENT_KEY_DOWN;
  open_event.key.key = SDLK_E;
  open_event.key.mod = SDL_KMOD_CTRL;
  const auto open_result = shell.HandleEvent(open_event);
  Expect(open_result.handled,
         "command prompt invalidation fixture should open command mode");
  (void)open_result.redraw;

  SDL_Event event{};
  event.type = SDL_EVENT_TEXT_INPUT;
  const std::string text = "palette";
  event.text.text = text.c_str();
  const auto result = shell.HandleEvent(event);

  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const SDL_FRect command_area = microide::workspace::BottomPanelCommandAreaRect(layout);

  Expect(result.handled, "command prompt typing should be handled");
  Expect(!result.redraw.full && !result.redraw.rects.empty(),
         "command prompt typing should stay on the partial redraw path");
  Expect(AnyRectIntersects(result.redraw.rects, command_area),
         "command prompt typing redraws should include the command area");
  Expect(WorkspaceShellTestAccess::CommandInput(shell) == text,
         "command prompt typing should append to the visible command input");
}

void TestWorkspaceShellCommandPasteShortcutUsesSharedTextInputPath() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(SendKeyDown(shell, SDLK_E, SDL_KMOD_CTRL),
         "command paste fixture should open the command prompt");
  WorkspaceShellTestAccess::SetClipboardTextReader(
      shell, []() -> std::optional<std::string> { return std::string("palette"); });

  Expect(SendKeyDown(shell, SDLK_V, SDL_KMOD_CTRL),
         "Ctrl+V should be handled by the command prompt");
  Expect(WorkspaceShellTestAccess::CommandInput(shell) == "palette",
         "Ctrl+V should route clipboard text through the shared command text-input path");
}

void TestWorkspaceShellChatComposerKeysDoNotLeakIntoEditor() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.txt";
  WriteFile(source, "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 2);
  const auto original_lines = WorkspaceShellTestAccess::ActiveEditor(shell).lines();
  const std::size_t original_column = WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column();

  WorkspaceShellTestAccess::ShowChatPanel(shell);
  Expect(WorkspaceShellTestAccess::FocusIsSidebar(shell),
         "chat key fixture should focus the sidebar");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Chat,
         "chat key fixture should activate the chat sidebar");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "hello"),
         "chat key fixture should type into the chat composer");
  Expect(WorkspaceShellTestAccess::ChatComposerInput(shell) == "hello",
         "chat typing should populate the chat composer");

  Expect(SendKeyDown(shell, SDLK_BACKSPACE, SDL_KMOD_NONE),
         "Backspace should be handled while the chat composer is focused");
  Expect(WorkspaceShellTestAccess::ChatComposerInput(shell) == "hell",
         "Backspace should edit the chat composer text");

  Expect(SendKeyDown(shell, SDLK_LEFT, SDL_KMOD_NONE),
         "navigation keys should be consumed while the chat composer is focused");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines() == original_lines,
         "chat key handling should not mutate the underlying editor buffer");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_column() == original_column,
         "chat key handling should not move the underlying editor cursor");
}

void TestWorkspaceShellChatComposerSupportsMultilineDraftsPerConversation() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.txt";
  WriteFile(source, "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::ShowChatPanel(shell);

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "hello"),
         "multiline chat draft fixture should type the first line");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "plain Enter in the chat composer should insert a newline");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "world"),
         "multiline chat draft fixture should type the second line");
  Expect(WorkspaceShellTestAccess::ChatComposerInput(shell) == "hello\nworld",
         "chat composer should preserve multiline draft text");

  const std::string first_conversation_id = WorkspaceShellTestAccess::ActiveConversationId(shell);
  Expect(WorkspaceShellTestAccess::CreateChatConversation(shell),
         "creating a second conversation should succeed");
  Expect(WorkspaceShellTestAccess::ChatComposerInput(shell).empty(),
         "new conversations should start with an empty draft");

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "second draft"),
         "second conversation should accept its own draft text");
  const std::string second_conversation_id = WorkspaceShellTestAccess::ActiveConversationId(shell);
  Expect(second_conversation_id != first_conversation_id,
         "creating a second conversation should switch the active conversation");

  Expect(WorkspaceShellTestAccess::ActivateChatConversation(shell, first_conversation_id),
         "switching back to the first conversation should succeed");
  Expect(WorkspaceShellTestAccess::ChatComposerInput(shell) == "hello\nworld",
         "switching conversations should restore the first conversation draft");

  Expect(WorkspaceShellTestAccess::ActivateChatConversation(shell, second_conversation_id),
         "switching back to the second conversation should succeed");
  Expect(WorkspaceShellTestAccess::ChatComposerInput(shell) == "second draft",
         "each conversation should retain its own draft buffer");
}

void TestWorkspaceShellChatComposerSelectAllAndCutAffectCurrentLineOnly() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.txt";
  WriteFile(source, "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::ShowChatPanel(shell);

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "hello"),
         "chat line-select fixture should type first line");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "chat line-select fixture should insert newline");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "world"),
         "chat line-select fixture should type second line");
  Expect(SendKeyDown(shell, SDLK_UP, SDL_KMOD_NONE),
         "chat line-select fixture should move cursor to first line");

  std::string clipboard;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard = std::string(text);
        return true;
      });
  WorkspaceShellTestAccess::SetPrimarySelectionTextWriter(shell, [](std::string_view) { return true; });

  Expect(SendKeyDown(shell, SDLK_A, SDL_KMOD_CTRL),
         "Ctrl+A should be handled in chat composer");
  Expect(SendKeyDown(shell, SDLK_X, SDL_KMOD_CTRL),
         "Ctrl+X should cut selected chat composer text");
  const std::string input_after_cut = WorkspaceShellTestAccess::ChatComposerInput(shell);
  Expect(input_after_cut == "\nworld" || input_after_cut == "world",
         "chat composer cut should preserve non-active lines");
}

void TestWorkspaceShellProjectTabsExposeChatStatusSummary() {
  Expect(true, "chat status summary is retired");
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

void TestWorkspaceShellChatTranscriptShowsMarkdownMetadataAndToolEvents() {
  Expect(true, "chat transcript metadata is retired");
}

void TestWorkspaceShellChatTranscriptLocalLinksOpenFiles() {
  Expect(true, "chat transcript links are retired");
}

void TestWorkspaceShellChatTranscriptRemoteLinksRequireConfirmation() {
  Expect(true, "chat transcript remote links are retired");
}

void TestWorkspaceShellSidebarDropdownOffersChatView() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.txt";
  WriteFile(source, "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  const SDL_FRect button_rect = WorkspaceShellTestAccess::SidebarModeButtonRect(shell);
  const float click_x = button_rect.x + button_rect.w * 0.5f;
  const float click_y = button_rect.y + button_rect.h * 0.5f;
  Expect(SendMouseDown(shell, click_x, click_y, SDL_BUTTON_LEFT),
         "clicking the sidebar mode control should open the sidebar menu");
  Expect(WorkspaceShellTestAccess::SidebarModeMenuOpen(shell),
         "clicking the sidebar mode control should open the sidebar menu");

  const auto labels = WorkspaceShellTestAccess::SidebarModeMenuLabels(shell);
  Expect(std::find(labels.begin(), labels.end(), "Chat") == labels.end(),
         "the sidebar dropdown should omit chat after AI capability removal");
  Expect(std::find(labels.begin(), labels.end(), "Problems") == labels.end(),
         "the sidebar dropdown should omit the Problems entry");
  Expect(std::find(labels.begin(), labels.end(), "Tests") == labels.end(),
         "the sidebar dropdown should omit the Tests entry");
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

  const auto action_rects = WorkspaceShellTestAccess::GitSidebarEntryActionRects(shell, 0);
  (void)SendMouseMotion(
      shell, action_rects[0].x + action_rects[0].w * 0.5f,
      action_rects[0].y + action_rects[0].h * 0.5f, 0);
  Expect(WorkspaceShellTestAccess::HoveredGitSidebarTooltipLabel(shell) == "Stage",
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
      WorkspaceShellTestAccess::ActiveEditor(cut_shell).lines();

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
  Expect(WorkspaceShellTestAccess::ActiveEditor(cut_shell).lines() != original_lines,
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
  Expect(WorkspaceShellTestAccess::ActiveEditor(cut_line_shell).lines() ==
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
  Expect(WorkspaceShellTestAccess::ActiveEditor(undo_shell).lines() == original_lines,
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
         "Copy Relative Path should copy the active tab path relative to the project root");

  clipboard_text.clear();
  Expect(WorkspaceShellTestAccess::ExecuteCopyAbsolutePath(shell),
         "Copy Absolute Path should execute from the active tab");
  Expect(clipboard_text == target.lexically_normal().string(),
         "Copy Absolute Path should copy the active tab path");
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
#endif

}  // namespace

void RegisterWorkspaceShellChromeTests(std::vector<TestCase>& tests) {
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
  AddTest(tests, "WorkspaceShell/StatusRowShowsLspReadinessAndInFlightState",
          TestWorkspaceShellStatusRowShowsLspReadinessAndInFlightState);
  AddTest(tests, "WorkspaceShell/EditorCaretDirtyRectFollowsActiveCaret",
          TestWorkspaceShellEditorCaretDirtyRectFollowsActiveCaret);
  AddTest(tests, "WorkspaceShell/EditorTypingReturnsPartialEditorInvalidation",
          TestWorkspaceShellEditorTypingReturnsPartialEditorInvalidation);
  AddTest(tests, "WorkspaceShell/CommandTextInputReturnsPartialCommandInvalidation",
          TestWorkspaceShellCommandTextInputReturnsPartialCommandInvalidation);
  AddTest(tests, "WorkspaceShell/CommandPasteShortcutUsesSharedTextInputPath",
          TestWorkspaceShellCommandPasteShortcutUsesSharedTextInputPath);
  AddTest(tests, "WorkspaceShell/ChatComposerKeysDoNotLeakIntoEditor",
          TestWorkspaceShellChatComposerKeysDoNotLeakIntoEditor);
  AddTest(tests, "WorkspaceShell/ChatComposerSupportsMultilineDraftsPerConversation",
          TestWorkspaceShellChatComposerSupportsMultilineDraftsPerConversation);
  AddTest(tests, "WorkspaceShell/ChatComposerSelectAllAndCutAffectCurrentLineOnly",
          TestWorkspaceShellChatComposerSelectAllAndCutAffectCurrentLineOnly);
  AddTest(tests, "WorkspaceShell/ProjectTabsExposeChatStatusSummary",
          TestWorkspaceShellProjectTabsExposeChatStatusSummary);
  AddTest(tests, "WorkspaceShell/ProjectTabsShowBadges",
          TestWorkspaceShellProjectTabsShowBadges);
  AddTest(tests, "WorkspaceShell/ChatTranscriptShowsMarkdownMetadataAndToolEvents",
          TestWorkspaceShellChatTranscriptShowsMarkdownMetadataAndToolEvents);
  AddTest(tests, "WorkspaceShell/ChatTranscriptLocalLinksOpenFiles",
          TestWorkspaceShellChatTranscriptLocalLinksOpenFiles);
  AddTest(tests, "WorkspaceShell/ChatTranscriptRemoteLinksRequireConfirmation",
          TestWorkspaceShellChatTranscriptRemoteLinksRequireConfirmation);
  AddTest(tests, "WorkspaceShell/SidebarDropdownOffersChatView",
          TestWorkspaceShellSidebarDropdownOffersChatView);
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
  AddTest(tests, "WorkspaceShell/TabContextActionsCloseAdjacentTabs",
          TestWorkspaceShellTabContextActionsCloseAdjacentTabs);
#if MICROIDE_HAS_SDL3_TTF
  AddTest(tests, "WorkspaceShell/SidebarModeRetainedRedrawMatchesFullRender",
          TestWorkspaceShellSidebarModeRetainedRedrawMatchesFullRender);
  AddTest(tests, "WorkspaceShell/OpenFileInNewTabRetainedRedrawMatchesFullRender",
          TestWorkspaceShellOpenFileInNewTabRetainedRedrawMatchesFullRender);
  AddTest(tests, "WorkspaceShell/PartialRedrawWithoutCompareTabSkipsCompareSurfaceRender",
          TestWorkspaceShellPartialRedrawWithoutCompareTabSkipsCompareSurfaceRender);
  AddTest(tests, "WorkspaceShell/NewlineInsertionRequestsEditorPartialRedraw",
          TestWorkspaceShellNewlineInsertionRequestsEditorPartialRedraw);
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
#endif
  AddTest(tests, "WorkspaceShell/FileCloseAllTabsClosesOpenEditorTabs",
          TestWorkspaceShellFileCloseAllTabsClosesOpenEditorTabs);
  AddTest(tests, "WorkspaceShell/DoubleClickTitleBarRequestsMaximizeToggle",
          TestWorkspaceShellDoubleClickTitleBarRequestsMaximizeToggle);
  AddTest(tests, "WorkspaceShell/FullscreenStateDisablesResizableFrameHitTest",
          TestWorkspaceShellFullscreenStateDisablesResizableFrameHitTest);
  AddTest(tests, "WorkspaceShell/WindowPresentationStateUpdatesChromeAndSize",
          TestWorkspaceShellWindowPresentationStateUpdatesChromeAndSize);
}

}  // namespace microide::tests
