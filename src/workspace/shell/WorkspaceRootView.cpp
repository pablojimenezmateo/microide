#include "workspace/shell/WorkspaceRootView.h"

#include <utility>

#include "util/PerformanceTrace.h"

namespace microide::workspace {

WorkspaceRootView::WorkspaceRootView(FrameOperations frame_operations, Views views)
    : frame_operations_(std::move(frame_operations)), views_(std::move(views)) {}

void WorkspaceRootView::Render(SDL_Renderer* renderer, int width, int height) {
  frame_operations_.prepare_render_frame(renderer, width, height);
  RenderPrepared(renderer, width, height);
}

void WorkspaceRootView::RenderPrepared(SDL_Renderer* renderer, int width, int height) {
  if (renderer == nullptr || width <= 0 || height <= 0) {
    return;
  }

  util::PerformanceTrace::Scope trace_scope("WorkspaceRootView::Render");
  const WorkspaceLayout layout = frame_operations_.compute_layout(width, height);
  std::optional<SDL_FRect> active_editor_pane_rect;
  frame_operations_.reset_visible_editor_blame_overlay();
  const bool draw_editor_caret = frame_operations_.should_draw_editor_caret();

  frame_operations_.render_frame_base(renderer, layout);
  views_.active_surface.Render(renderer, layout, draw_editor_caret, &active_editor_pane_rect);
  views_.chrome.Render(renderer, layout);
  views_.sidebar.Render(renderer, layout);
  views_.overlay.Render(renderer, layout);
  views_.panel.Render(renderer, layout);
  views_.menu.Render(renderer, layout);
  views_.prompt.Render(renderer, layout, active_editor_pane_rect);
}

}  // namespace microide::workspace
