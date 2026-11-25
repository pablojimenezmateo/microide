#include "workspace/render/WorkspaceShellRenderPrimitives.h"
#include "workspace/render/NotificationToastPaint.h"
#include "workspace/shell/WorkspaceShell.h"

#include <SDL3/SDL.h>

#include <algorithm>

#include "render/PaintElement.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "workspace/shell/WorkspaceShellBootstrapper.h"

namespace microide::workspace {

namespace {

bool RectContains(const SDL_FRect& outer, const SDL_FRect& inner) {
  return outer.w > 0.0f && outer.h > 0.0f &&
         inner.x >= outer.x && inner.y >= outer.y &&
         inner.x + inner.w <= outer.x + outer.w &&
         inner.y + inner.h <= outer.y + outer.h;
}

}  // namespace

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
      menu_hover_trace && frame_state_.menu_popups[static_cast<std::size_t>(MenuPopupSlot::Popup)].visible
          ? SDL_GetTicksNS()
          : 0;

  // "WorkspaceRootView::Render" is trace vocabulary, not a class reference: the
  // WorkspaceRootView indirection this clip path replaced is gone, but every perf
  // ledger, baseline and investigation in dev-docs/performance names these scopes,
  // so the labels stay put. Don't go looking for the class.
  util::PerformanceTrace::Scope trace_scope("WorkspaceRootView::Render");
  util::AddPerformanceCounter(util::PerfCounterId::RenderClipInvocations);
  const WorkspaceLayout& layout = *prepared_frame_layout_;

  EnsureClipFrameAndOverlayViewModels(layout);
  // Heavy subsystems (editor surface, window chrome, sidebar, debug pane, bottom
  // panel) walk their own scenes even when the SDL clip would mask their pixels,
  // so a partial clip runs only the passes that paint something it touches --
  // above all during menu hover, where the clips sit in the popup and the editor
  // render dominated per-event CPU. What each pass paints is prepared
  // (PreparedFrameState::paint_pass_extents): its layout region and every Chrome
  // unit it draws, the unit naming its pass where it is keyed. This was a list of
  // rects RenderClip assembled from live state -- the strips of each pane, a tab
  // drag's drop preview, a tree drag's ghost, the split dividers' tops -- each one
  // added after the QA verifier caught a pass skipped under a clip it painted.
  // A collapsed surface paints nothing, hinted or not (PaintPassExtents::Runs).
  const PaintPassExtents& passes = frame_state_.paint_pass_extents;
  bool skip_editor_surface = !passes.Runs(PaintPass::Editor, dirty_rect_hint);
  const bool skip_window_chrome = !passes.Runs(PaintPass::WindowChrome, dirty_rect_hint);
  bool skip_sidebar = !passes.Runs(PaintPass::Sidebar, dirty_rect_hint);
  bool skip_right_pane = !passes.Runs(PaintPass::RightPane, dirty_rect_hint);
  bool skip_bottom_panel = !passes.Runs(PaintPass::BottomPanel, dirty_rect_hint);
  // A clip wholly inside one open menu's card repaints the card alone: the card
  // is opaque over the editor, sidebar and panel, whose pixels the scene texture
  // keeps. One card, not their bounding box: a tall menu's box with the bar's
  // covers the editor, and a caret blink under an open Edit menu painted the
  // window background over it. The cards as this frame paints them (MenuPopupPaint).
  bool contained_in_chrome_union = false;
  std::optional<SDL_FRect> traced_chrome_union;
  if (dirty_rect_hint.has_value()) {
    for (const MenuPopupPaint& popup : frame_state_.menu_popups) {
      if (popup.visible && RectContains(popup.rect, *dirty_rect_hint)) {
        skip_editor_surface = true;
        skip_sidebar = true;
        skip_right_pane = true;
        skip_bottom_panel = true;
        contained_in_chrome_union = true;
        traced_chrome_union = popup.rect;
        break;
      }
    }
  }

  // Each surface gets its own scope: this function is the top of the per-frame
  // paint and every call below it is a whole subsystem, so without them the
  // ranked summary charges the lot to `WorkspaceRootView::Render` self time and
  // says only "painting is slow".
  {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::FrameBase");
    RenderFrameBase(renderer, layout);
  }
  // Each skippable pass draws inside what it declared (render::PaintPassScope,
  // checked while overrun recording is on: the QA redraw verifier and the redraw
  // tests). A draw outside it is a pixel a clip that skipped the pass leaves stale.
  const auto pass_scope = [&](PaintPass paint_pass, std::string_view name) {
    return render::PaintPassScope(renderer, name, &PaintPassExtent::CoversThunk,
                                  &passes.extents[static_cast<std::size_t>(paint_pass)]);
  };
  if (!skip_editor_surface) {
    const render::PaintPassScope pass = pass_scope(PaintPass::Editor, "editor");
    {
      util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::EditorSurface");
      RenderActiveWorkspaceSurface(renderer, layout, prepared_frame_draw_editor_caret_);
    }
    PaintCard(renderer, frame_state_.editor_hover_card, theme_, text_renderer_);
    PaintCard(renderer, frame_state_.signature_card, theme_, text_renderer_);
  }
  if (!skip_window_chrome) {
    const render::PaintPassScope pass = pass_scope(PaintPass::WindowChrome, "window-chrome");
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::WindowChrome");
    RenderWindowChrome(renderer, layout);
  }
  if (!skip_sidebar) {
    const render::PaintPassScope pass = pass_scope(PaintPass::Sidebar, "sidebar");
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::Sidebar");
    RenderSidebarSurface(renderer, layout);
  }
  if (!skip_right_pane) {
    const render::PaintPassScope pass = pass_scope(PaintPass::RightPane, "right-pane");
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::DebugPane");
    RenderDebugPaneSurface(renderer, layout);
  }
  if (!skip_bottom_panel) {
    const render::PaintPassScope pass = pass_scope(PaintPass::BottomPanel, "bottom-panel");
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::BottomPanel");
    RenderBottomPanelSurface(renderer, layout);
  }
  // After the panel: an overlay taller than the editor area (the palette, a quick
  // pick at a large ui.scale or in a short window) reaches down over the panel,
  // and painted before it, the panel's tabs and terminal covered its lower rows
  // in every frame (QA monkey).
  {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::Overlay");
    RenderOverlaySurface(renderer, layout, *clip_cached_overlay_vm_);
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::TooltipAndMenus");
    PaintCard(renderer, frame_state_.tooltip_card, theme_, text_renderer_);
    RenderMenuPopups(renderer, layout);
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::StatusBar");
    RenderStatusBar(renderer, layout);
  }
  {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::SettingsOverlay");
    RenderSettingsOverlay(renderer);
  }

  {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::TextInputAndNotifications");
    RenderPromptSurface(renderer, layout);
    PaintCard(renderer, frame_state_.text_input_marks, theme_, text_renderer_);
    PaintCard(renderer, frame_state_.text_input_caret, theme_, text_renderer_);
    RenderDirtyPromptSurface(renderer, layout);
    PaintNotificationToasts(renderer, frame_state_, theme_, text_renderer_);
  }

  if (trace_start_ns != 0) {
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
