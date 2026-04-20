#include "workspace/WorkspaceViewTree.h"

#include <utility>

namespace microide::workspace {

WorkspaceActiveSurfaceView::WorkspaceActiveSurfaceView(Operations operations)
    : operations_(std::move(operations)) {}

void WorkspaceActiveSurfaceView::Render(SDL_Renderer* renderer,
                                        const WorkspaceLayout& layout,
                                        bool draw_editor_caret,
                                        std::optional<SDL_FRect>* active_editor_pane_rect) const {
  operations_.render(renderer, layout, draw_editor_caret, active_editor_pane_rect);
  operations_.refresh_hover_if_needed();
  operations_.render_hover_popup(renderer);
}

WorkspaceChromeView::WorkspaceChromeView(RenderFn render) : render_(std::move(render)) {}

void WorkspaceChromeView::Render(SDL_Renderer* renderer, const WorkspaceLayout& layout) const {
  render_(renderer, layout);
}

WorkspaceSidebarView::WorkspaceSidebarView(RenderFn render) : render_(std::move(render)) {}

void WorkspaceSidebarView::Render(SDL_Renderer* renderer, const WorkspaceLayout& layout) const {
  render_(renderer, layout);
}

WorkspaceOverlayView::WorkspaceOverlayView(RenderFn render) : render_(std::move(render)) {}

void WorkspaceOverlayView::Render(SDL_Renderer* renderer, const WorkspaceLayout& layout) const {
  render_(renderer, layout);
}

WorkspacePanelView::WorkspacePanelView(Operations operations)
    : operations_(std::move(operations)) {}

void WorkspacePanelView::Render(SDL_Renderer* renderer, const WorkspaceLayout& layout) const {
  operations_.render(renderer, layout, operations_.active_terminal_line_count());
}

WorkspaceMenuView::WorkspaceMenuView(RenderFn render) : render_(std::move(render)) {}

void WorkspaceMenuView::Render(SDL_Renderer* renderer, const WorkspaceLayout& layout) const {
  render_(renderer, layout);
}

WorkspacePromptView::WorkspacePromptView(RenderFn render) : render_(std::move(render)) {}

void WorkspacePromptView::Render(SDL_Renderer* renderer,
                                 const WorkspaceLayout& layout,
                                 const std::optional<SDL_FRect>& active_editor_pane_rect) const {
  render_(renderer, layout, active_editor_pane_rect);
}

}  // namespace microide::workspace
