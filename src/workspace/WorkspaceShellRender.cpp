#include "workspace/WorkspaceShell.h"

#include <SDL3/SDL.h>

#include <algorithm>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceShellBootstrapper.h"

namespace microide::workspace {

namespace {

bool RectsIntersect(const SDL_FRect& a, const SDL_FRect& b) {
  return a.w > 0.0f && a.h > 0.0f && b.w > 0.0f && b.h > 0.0f &&
         a.x < b.x + b.w && b.x < a.x + a.w &&
         a.y < b.y + b.h && b.y < a.y + a.h;
}

bool RectContains(const SDL_FRect& outer, const SDL_FRect& inner) {
  return outer.w > 0.0f && outer.h > 0.0f &&
         inner.x >= outer.x && inner.y >= outer.y &&
         inner.x + inner.w <= outer.x + outer.w &&
         inner.y + inner.h <= outer.y + outer.h;
}

// Returns true when the dirty rect, if provided, doesn't overlap `surface`.
// A null hint means "render everything" (full redraw).
bool DirtyHintSkips(const std::optional<SDL_FRect>& hint, const SDL_FRect& surface) {
  return hint.has_value() && !RectsIntersect(*hint, surface);
}

}  // namespace

WorkspaceRootView WorkspaceShell::MakeRootView() { return Bootstrapper(*this).BuildRootView(); }

void WorkspaceShell::RenderClip(const FrameToken& frame_token,
                                SDL_Renderer* renderer,
                                int width,
                                int height,
                                std::optional<SDL_FRect> dirty_rect_hint) {
  if (renderer == nullptr || width <= 0 || height <= 0 || prepared_frame_layout_ == std::nullopt ||
      frame_token.frame_id() == 0 || frame_token.frame_id() != prepared_frame_id_) {
    return;
  }

  // Set MICROIDE_TRACE_MENU_HOVER=1 to log per-RenderClip timing/skip decisions
  // when a menu is open. One line per partial-render clip rect; quiet when
  // unset. Used to verify which subsystems actually run during menu hover.
  static const bool menu_hover_trace =
      util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_MENU_HOVER");
  const Uint64 trace_start_ns =
      menu_hover_trace && context_.menu_state.menu_bar_open ? SDL_GetTicksNS() : 0;

  util::PerformanceTrace::Scope trace_scope("WorkspaceRootView::Render");
  util::AddPerformanceCounter(util::PerfCounterId::RenderClipInvocations);
  const WorkspaceLayout& layout = *prepared_frame_layout_;
  std::optional<SDL_FRect> active_editor_pane_rect;
  editor_blame_overlay_service_.ClearVisibleOverlay();

  EnsureClipFrameAndOverlayViewModels(layout);
  // Heavy subsystems (editor surface, sidebar, bottom panel, settings overlay)
  // walk their own scenes top-to-bottom even when SDL clip rects would mask the
  // pixels. Threading the dirty rect lets us short-circuit these on partial
  // redraws — most importantly during menu hover, where the dirty rects sit in
  // the popup region and the editor render dominates per-event CPU.
  // Layout zeroes the sidebar/bottom panel rects when they're hidden, so
  // RectsIntersect (which requires positive w and h) already short-circuits
  // those — no need to peek at project state from a render TU.
  bool skip_editor_surface = DirtyHintSkips(dirty_rect_hint, layout.editor_surface);
  bool skip_sidebar = DirtyHintSkips(dirty_rect_hint, layout.sidebar);
  bool skip_right_pane = DirtyHintSkips(dirty_rect_hint, layout.right_pane);
  bool skip_bottom_panel = DirtyHintSkips(dirty_rect_hint, layout.bottom_panel);
  // Window chrome spans menu bar, project/editor tab strips, and the
  // breadcrumb row. Skip the whole chrome pass when none intersect — popup
  // hover dirty rects sit below the chrome strip and don't touch it.
  bool skip_window_chrome =
      DirtyHintSkips(dirty_rect_hint, layout.menu_bar) &&
      DirtyHintSkips(dirty_rect_hint, layout.project_tab_strip) &&
      DirtyHintSkips(dirty_rect_hint, layout.tab_strip) &&
      DirtyHintSkips(dirty_rect_hint, layout.breadcrumb);
  // Popups float over the editor area (they drop down from the menu bar),
  // so an intersection check against editor_surface alone would still drag
  // in the editor render every time a popup row dirty rect lands below the
  // chrome strip. When the dirty rect sits fully inside the open menu/chrome
  // union, only the menu popup needs repainting — the editor/sidebar/panel
  // contents underneath are already retained in the scene texture.
  // (Window chrome stays gated by its own intersection check above so
  // menu-bar item highlight changes still trigger a chrome repaint.)
  bool contained_in_chrome_union = false;
  std::optional<SDL_FRect> traced_chrome_union;
  if (dirty_rect_hint.has_value()) {
    if (const auto chrome_union = CurrentChromeRedrawRect();
        chrome_union.has_value() && RectContains(*chrome_union, *dirty_rect_hint)) {
      skip_editor_surface = true;
      skip_sidebar = true;
      skip_right_pane = true;
      skip_bottom_panel = true;
      contained_in_chrome_union = true;
      traced_chrome_union = chrome_union;
    } else if (chrome_union.has_value()) {
      traced_chrome_union = chrome_union;
    }
  }

  // Each surface gets its own scope: this function is the top of the per-frame
  // paint and every call below it is a whole subsystem, so without them the
  // ranked summary charges the lot to `WorkspaceRootView::Render` self time and
  // says only "painting is slow".
  {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::FrameBase");
    RenderFrameBase(renderer, layout, *clip_cached_frame_vm_);
  }
  if (!skip_editor_surface) {
    {
      util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::EditorSurface");
      RenderActiveWorkspaceSurface(renderer, layout, frame_token, prepared_frame_draw_editor_caret_,
                                   &active_editor_pane_rect, *clip_cached_frame_vm_,
                                   *clip_cached_overlay_vm_);
    }
    if (editor_hover_refresh_pending_ && last_mouse_position_valid_) {
      util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::RefreshHover");
      UpdateEditorHover(last_mouse_x_, last_mouse_y_);
      editor_hover_refresh_pending_ = false;
    }
    RenderEditorHoverPopup(renderer);
    MaybeExpireSignatureHelp();
    RenderSignatureHelpPopup(renderer);
  }
  if (!skip_window_chrome) {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::WindowChrome");
    RenderWindowChrome(renderer, layout);
  }
  if (!skip_sidebar) {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::Sidebar");
    RenderSidebarSurface(renderer, layout);
  }
  if (!skip_right_pane) {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::DebugPane");
    RenderDebugPaneSurface(renderer, layout);
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::Overlay");
    RenderOverlaySurface(renderer, layout, *clip_cached_overlay_vm_);
  }
  if (!skip_bottom_panel) {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::BottomPanel");
    RenderBottomPanelSurface(
        renderer, layout,
        ActiveTerminalTab() != nullptr ? ActiveTerminalTab()->session.LineCount() : std::size_t{0});
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::TooltipAndMenus");
    RenderHoverTooltip(renderer, layout);
    RenderMenuPopups(renderer, layout);
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::StatusBar");
    RenderStatusBar(renderer, layout);
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::SettingsOverlay");
    RenderSettingsOverlay(renderer, layout);
  }

  {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::TextInputAndNotifications");
    SDL_Window* render_window = SDL_GetRenderWindow(renderer);
    const auto active_text_input_visual =
        BuildActiveTextInputVisual(layout, active_editor_pane_rect);
    RenderPromptSurface(renderer, layout, active_text_input_visual);
    RenderSingleLineTextSelection(renderer, active_text_input_visual);
    RenderActiveTextInputCaret(renderer, active_text_input_visual);
    RenderTextComposition(renderer, active_text_input_visual);
    UpdateTextInputArea(renderer, render_window, active_text_input_visual);
    RenderDirtyPromptSurface(renderer, layout);
    RenderNotifications(renderer, layout);
  }

  if (menu_hover_trace && context_.menu_state.menu_bar_open) {
    const double elapsed_ms =
        static_cast<double>(SDL_GetTicksNS() - trace_start_ns) / 1'000'000.0;
    char hint_buf[96] = "(none)";
    if (dirty_rect_hint.has_value()) {
      SDL_snprintf(hint_buf, sizeof(hint_buf), "(%.0f,%.0f %.0fx%.0f)",
                   dirty_rect_hint->x, dirty_rect_hint->y, dirty_rect_hint->w,
                   dirty_rect_hint->h);
    }
    char chrome_buf[96] = "(none)";
    if (traced_chrome_union.has_value()) {
      SDL_snprintf(chrome_buf, sizeof(chrome_buf), "(%.0f,%.0f %.0fx%.0f)",
                   traced_chrome_union->x, traced_chrome_union->y, traced_chrome_union->w,
                   traced_chrome_union->h);
    }
    SDL_Log("MICROIDE_MENU_HOVER render %.2fms hint=%s chrome=%s contained=%d "
            "skip[editor=%d chrome=%d sidebar=%d panel=%d]",
            elapsed_ms, hint_buf, chrome_buf, contained_in_chrome_union ? 1 : 0,
            skip_editor_surface ? 1 : 0, skip_window_chrome ? 1 : 0,
            skip_sidebar ? 1 : 0, skip_bottom_panel ? 1 : 0);
  }

}

void WorkspaceShell::Render(SDL_Renderer* renderer, int width, int height) {
  const FrameToken frame_token = PrepareFrameOnce(renderer, width, height);
  RenderClip(frame_token, renderer, width, height);
}

void WorkspaceShell::RenderPrepared(SDL_Renderer* renderer, int width, int height) {
  const FrameToken frame_token = FrameToken{prepared_frame_id_, FrameToken::VisibleLineRange{}};
  if (prepared_frame_layout_ == std::nullopt || prepared_frame_id_ == 0) {
    Render(renderer, width, height);
    return;
  }
  RenderClip(frame_token, renderer, width, height);
}

}  // namespace microide::workspace
