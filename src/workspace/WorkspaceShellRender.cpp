#include "workspace/WorkspaceShell.h"

#include <optional>

#include "util/PerformanceTrace.h"

namespace microide::workspace {

void WorkspaceShell::Render(SDL_Renderer* renderer, int width, int height) {
  PrepareRenderFrame(renderer, width, height);
  RenderPrepared(renderer, width, height);
}

void WorkspaceShell::RenderPrepared(SDL_Renderer* renderer, int width, int height) {
  if (renderer == nullptr || width <= 0 || height <= 0) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("WorkspaceShell::Render");
  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(width), static_cast<float>(height), surface_.sidebar_visible,
                    BottomPanelVisible(), surface_.sidebar_width, surface_.bottom_panel_height);
  SDL_Window* render_window = SDL_GetRenderWindow(renderer);
  const std::size_t terminal_line_count =
      ActiveTerminalTab() != nullptr ? ActiveTerminalTab()->session.LineCount() : 0;
  std::optional<SDL_FRect> active_editor_pane_rect;
  visible_editor_blame_overlay_.reset();
  const bool draw_editor_caret =
      CaretVisibleNow() &&
      !(CurrentTextInputSurface() == TextInputSurface::Editor && !text_composition_.text.empty());

  RenderFrameBase(renderer, layout);
  RenderActiveWorkspaceSurface(renderer, layout, draw_editor_caret, &active_editor_pane_rect);

  if (editor_hover_refresh_pending_ && last_mouse_position_valid_) {
    UpdateEditorHover(last_mouse_x_, last_mouse_y_);
    editor_hover_refresh_pending_ = false;
  }

  const auto active_text_input_visual =
      BuildActiveTextInputVisual(layout, active_editor_pane_rect);
  RenderEditorHoverPopup(renderer);
  RenderWindowChrome(renderer, layout);
  RenderSidebarSurface(renderer, layout);
  RenderOverlaySurface(renderer, layout);
  RenderBottomPanelSurface(renderer, layout, terminal_line_count);
  RenderMenuPopups(renderer, layout);
  RenderPromptSurface(renderer, layout, active_text_input_visual);
  RenderTextComposition(renderer, active_text_input_visual);
  UpdateTextInputArea(renderer, render_window, active_text_input_visual);
  RenderDirtyPromptSurface(renderer, layout);
}

}  // namespace microide::workspace
