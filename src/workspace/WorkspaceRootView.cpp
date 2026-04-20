#include "workspace/WorkspaceRootView.h"

#include <utility>

#include "util/PerformanceTrace.h"

namespace microide::workspace {

WorkspaceRootView::WorkspaceRootView(Operations operations)
    : operations_(std::move(operations)) {}

void WorkspaceRootView::Render(SDL_Renderer* renderer, int width, int height) {
  operations_.prepare_render_frame(renderer, width, height);
  RenderPrepared(renderer, width, height);
}

void WorkspaceRootView::RenderPrepared(SDL_Renderer* renderer, int width, int height) {
  if (renderer == nullptr || width <= 0 || height <= 0) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("WorkspaceRootView::Render");
  const WorkspaceLayout layout = operations_.compute_layout(width, height);
  const std::size_t terminal_line_count = operations_.active_terminal_line_count();
  std::optional<SDL_FRect> active_editor_pane_rect;
  operations_.reset_visible_editor_blame_overlay();
  const bool draw_editor_caret = operations_.should_draw_editor_caret();

  operations_.render_frame_base(renderer, layout);
  operations_.render_active_workspace_surface(renderer, layout, draw_editor_caret,
                                              &active_editor_pane_rect);
  operations_.refresh_hover_if_needed();
  operations_.render_editor_hover_popup(renderer);
  operations_.render_window_chrome(renderer, layout);
  operations_.render_sidebar_surface(renderer, layout);
  operations_.render_overlay_surface(renderer, layout);
  operations_.render_bottom_panel_surface(renderer, layout, terminal_line_count);
  operations_.render_menu_popups(renderer, layout);
  operations_.render_prompt_stack(renderer, layout, active_editor_pane_rect);
}

}  // namespace microide::workspace
