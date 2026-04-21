#include "TestSupport.h"

#include "TerminalSessionTestAccess.h"
#include "workspace/WorkspaceShellTesting.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const std::vector<std::string> labels = WorkspaceShellTestAccess::VisibleMenuBarLabels(shell);
  Expect(std::find(labels.begin(), labels.end(), "Project") == labels.end(),
         "menu bar should omit the removed Project menu");
  Expect(std::find(labels.begin(), labels.end(), "Terminal") == labels.end(),
         "menu bar should omit the removed Terminal menu");
  Expect(std::find(labels.begin(), labels.end(), "Help") == labels.end(),
         "menu bar should omit the removed Help menu");
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
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true);

  Expect(WorkspaceShellTestAccess::WindowHitTest(shell, 640.0f, 10.0f) == SDL_HITTEST_DRAGGABLE,
         "empty title-bar hit testing should hand borderless dragging back to the window manager");
  Expect(WorkspaceShellTestAccess::WindowDragRegionContains(shell, 640.0f, 10.0f),
         "empty title-bar space should still be eligible for window dragging");
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(shell, 640.0f, 10.0f, SDL_BUTTON_LEFT, 2),
         "double-clicking an empty title-bar region should be handled");
  Expect(WorkspaceShellTestAccess::ConsumeWindowAction(shell) ==
             WorkspaceShell::WindowAction::ToggleMaximize,
         "double-clicking the title bar should request the same maximize toggle as the chrome button");
}

void TestWorkspaceShellFullscreenStateDisablesResizableFrameHitTest() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::SetWindowChromeEnabled(shell, true, false, true);

  Expect(WorkspaceShellTestAccess::WindowHitTest(shell, 1.0f, 1.0f) == SDL_HITTEST_NORMAL,
         "fullscreen chrome state should not expose resize hit targets");
}

void TestWorkspaceShellWindowPresentationStateUpdatesChromeAndSize() {
  WorkspaceShell shell;
  shell.SetWindowPresentationState(WorkspaceShell::WindowPresentationState{
      .logical_width = 1280,
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

  Expect(WorkspaceShellTestAccess::WindowHitTest(shell, 1.0f, 1.0f) == SDL_HITTEST_DRAGGABLE,
         "maximized presentation state should keep the title bar draggable without exposing resize hit targets");
  Expect(WorkspaceShellTestAccess::WindowDragRegionContains(shell, 640.0f, 10.0f),
         "presentation state should keep the title bar draggable");
}

void TestWorkspaceShellMenuBarHoverSwitchesActiveMenu() {
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  const auto file_rect = WorkspaceShellTestAccess::MenuBarItemRect(shell, "File");
  const auto edit_rect = WorkspaceShellTestAccess::MenuBarItemRect(shell, "Edit");
  Expect(file_rect.has_value(), "menu hover fixture should expose a File menu item");
  Expect(edit_rect.has_value(), "menu hover fixture should expose an Edit menu item");

  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(
             shell, file_rect->x + file_rect->w * 0.5f, file_rect->y + file_rect->h * 0.5f,
             SDL_BUTTON_LEFT),
         "clicking the File menu should be handled");
  Expect(WorkspaceShellTestAccess::FileMenuOpen(shell),
         "clicking the File menu should open the File popup");

  Expect(WorkspaceShellTestAccess::HandleMouseMotion(
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

  const std::optional<SDL_FRect> caret_rect = WorkspaceShellTestAccess::CurrentCaretDirtyRect(shell);
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
  (void)WorkspaceShellTestAccess::ConsumePendingRenderInvalidation(shell);

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

void TestWorkspaceShellShortcutEditActionsReturnEditorInvalidation() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "main.cpp";
  WriteFile(source, "alpha\nbeta\n");

  const auto configure_shell = [&](WorkspaceShell& shell) {
    WorkspaceShellTestAccess::SetProjectRoot(shell, root);
    WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
    WorkspaceShellTestAccess::OpenFile(shell, source);
    (void)WorkspaceShellTestAccess::ConsumePendingRenderInvalidation(shell);
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
  const std::filesystem::path left = root / "left.txt";
  const std::filesystem::path right = root / "right.txt";
  WriteFile(left, "left\n");
  WriteFile(right, "right\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, left);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, right),
         "tab context-menu fixture should open a second editor tab");

  const SDL_FRect tab_rect = WorkspaceShellTestAccess::EditorTabRect(shell, 0);
  Expect(WorkspaceShellTestAccess::HandleMouseButtonDown(
             shell, tab_rect.x + tab_rect.w * 0.5f, tab_rect.y + tab_rect.h * 0.5f,
             SDL_BUTTON_RIGHT),
         "right-clicking an editor tab should be handled");
  Expect(WorkspaceShellTestAccess::EditorTabContextMenuOpen(shell),
         "right-clicking an editor tab should open the editor tab context menu");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == 0,
         "right-clicking an editor tab should retarget the active tab before menu actions run");
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
  const auto redraw = WorkspaceShellTestAccess::ConsumePendingRenderInvalidation(retained_shell);
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
    (void)WorkspaceShellTestAccess::ConsumePendingRenderInvalidation(shell);
  };

  WorkspaceShell retained_shell;
  configure_shell(retained_shell);

  SoftwareCanvas retained_canvas(kCanvasWidth, kCanvasHeight);
  retained_shell.Render(retained_canvas.renderer(), kCanvasWidth, kCanvasHeight);

  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(retained_shell, beta),
         "tab redraw regression fixture should open a second file");
  const auto redraw = WorkspaceShellTestAccess::ConsumePendingRenderInvalidation(retained_shell);
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
    (void)WorkspaceShellTestAccess::ConsumePendingRenderInvalidation(shell);
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
    (void)WorkspaceShellTestAccess::ConsumePendingRenderInvalidation(shell);
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
#endif

}  // namespace

void RegisterWorkspaceShellChromeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/MenuBarOmitsRemovedMenus",
          TestWorkspaceShellMenuBarOmitsRemovedMenus);
  AddTest(tests, "WorkspaceShell/MenuBarHoverSwitchesActiveMenu",
          TestWorkspaceShellMenuBarHoverSwitchesActiveMenu);
  AddTest(tests, "WorkspaceShell/MenuEventsReturnPartialChromeInvalidation",
          TestWorkspaceShellMenuEventsReturnPartialChromeInvalidation);
  AddTest(tests, "WorkspaceShell/EditorCaretDirtyRectFollowsActiveCaret",
          TestWorkspaceShellEditorCaretDirtyRectFollowsActiveCaret);
  AddTest(tests, "WorkspaceShell/EditorTypingReturnsPartialEditorInvalidation",
          TestWorkspaceShellEditorTypingReturnsPartialEditorInvalidation);
  AddTest(tests, "WorkspaceShell/CommandTextInputReturnsPartialCommandInvalidation",
          TestWorkspaceShellCommandTextInputReturnsPartialCommandInvalidation);
  AddTest(tests, "WorkspaceShell/ShortcutEditActionsReturnEditorInvalidation",
          TestWorkspaceShellShortcutEditActionsReturnEditorInvalidation);
  AddTest(tests, "WorkspaceShell/EditorTabRightClickOpensContextMenu",
          TestWorkspaceShellEditorTabRightClickOpensContextMenu);
  AddTest(tests, "WorkspaceShell/TabContextActionsCloseAdjacentTabs",
          TestWorkspaceShellTabContextActionsCloseAdjacentTabs);
#if MICROIDE_HAS_SDL3_TTF
  AddTest(tests, "WorkspaceShell/SidebarModeRetainedRedrawMatchesFullRender",
          TestWorkspaceShellSidebarModeRetainedRedrawMatchesFullRender);
  AddTest(tests, "WorkspaceShell/OpenFileInNewTabRetainedRedrawMatchesFullRender",
          TestWorkspaceShellOpenFileInNewTabRetainedRedrawMatchesFullRender);
  AddTest(tests, "WorkspaceShell/SidebarResizeRequestsFullRedrawAndMatchesFullRender",
          TestWorkspaceShellSidebarResizeRequestsFullRedrawAndMatchesFullRender);
  AddTest(tests, "WorkspaceShell/BottomPanelResizeRequestsFullRedrawAndSettleFrames",
          TestWorkspaceShellBottomPanelResizeRequestsFullRedrawAndSettleFrames);
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
