#include "workspace/WorkspaceShell.h"

#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceShellBootstrapper.h"

namespace microide::workspace {

WorkspaceRootView WorkspaceShell::MakeRootView() { return Bootstrapper(*this).BuildRootView(); }

void WorkspaceShell::RenderClip(const FrameToken& frame_token,
                                SDL_Renderer* renderer,
                                int width,
                                int height) {
  if (renderer == nullptr || width <= 0 || height <= 0 || prepared_frame_layout_ == std::nullopt ||
      frame_token.frame_id_ == 0 || frame_token.frame_id_ != prepared_frame_id_) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("WorkspaceRootView::Render");
  const WorkspaceLayout& layout = *prepared_frame_layout_;
  std::optional<SDL_FRect> active_editor_pane_rect;
  visible_editor_blame_overlay_.reset();

  RenderFrameBase(renderer, layout);
  RenderActiveWorkspaceSurface(renderer, layout, frame_token, prepared_frame_draw_editor_caret_,
                               &active_editor_pane_rect);
  if (editor_hover_refresh_pending_ && last_mouse_position_valid_) {
    util::PerformanceTrace::Scope scope("WorkspaceRootView::Render::RefreshHover");
    UpdateEditorHover(last_mouse_x_, last_mouse_y_);
    editor_hover_refresh_pending_ = false;
  }
  RenderEditorHoverPopup(renderer);
  RenderWindowChrome(renderer, layout);
  RenderSidebarSurface(renderer, layout);
  RenderOverlaySurface(renderer, layout);
  RenderBottomPanelSurface(
      renderer, layout,
      ActiveTerminalTab() != nullptr ? ActiveTerminalTab()->session.LineCount() : std::size_t{0});
  RenderChromeTooltips(renderer, layout);
  RenderMenuPopups(renderer, layout);

  SDL_Window* render_window = SDL_GetRenderWindow(renderer);
  const auto active_text_input_visual =
      BuildActiveTextInputVisual(layout, active_editor_pane_rect);
  RenderPromptSurface(renderer, layout, active_text_input_visual);
  RenderSingleLineTextSelection(renderer, active_text_input_visual);
  RenderActiveTextInputCaret(renderer, active_text_input_visual);
  RenderTextComposition(renderer, active_text_input_visual);
  UpdateTextInputArea(renderer, render_window, active_text_input_visual);
  RenderDirtyPromptSurface(renderer, layout);
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
