#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <optional>

#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

class WorkspaceRootView {
 public:
  struct Operations {
    std::function<void(SDL_Renderer*, int, int)> prepare_render_frame;
    std::function<WorkspaceLayout(int, int)> compute_layout;
    std::function<std::size_t()> active_terminal_line_count;
    std::function<void()> reset_visible_editor_blame_overlay;
    std::function<bool()> should_draw_editor_caret;
    std::function<void(SDL_Renderer*, const WorkspaceLayout&)> render_frame_base;
    std::function<void(SDL_Renderer*,
                       const WorkspaceLayout&,
                       bool,
                       std::optional<SDL_FRect>*)>
        render_active_workspace_surface;
    std::function<void()> refresh_hover_if_needed;
    std::function<void(SDL_Renderer*)> render_editor_hover_popup;
    std::function<void(SDL_Renderer*, const WorkspaceLayout&)> render_window_chrome;
    std::function<void(SDL_Renderer*, const WorkspaceLayout&)> render_sidebar_surface;
    std::function<void(SDL_Renderer*, const WorkspaceLayout&)> render_overlay_surface;
    std::function<void(SDL_Renderer*, const WorkspaceLayout&, std::size_t)>
        render_bottom_panel_surface;
    std::function<void(SDL_Renderer*, const WorkspaceLayout&)> render_menu_popups;
    std::function<void(SDL_Renderer*,
                       const WorkspaceLayout&,
                       const std::optional<SDL_FRect>&)>
        render_prompt_stack;
  };

  explicit WorkspaceRootView(Operations operations);

  void Render(SDL_Renderer* renderer, int width, int height);
  void RenderPrepared(SDL_Renderer* renderer, int width, int height);

 private:
  Operations operations_;
};

}  // namespace microide::workspace
