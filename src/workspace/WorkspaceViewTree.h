#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <optional>

#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

class WorkspaceActiveSurfaceView {
 public:
  struct Operations {
    std::function<void(SDL_Renderer*,
                       const WorkspaceLayout&,
                       bool,
                       std::optional<SDL_FRect>*)>
        render;
    std::function<void()> refresh_hover_if_needed;
    std::function<void(SDL_Renderer*)> render_hover_popup;
  };

  explicit WorkspaceActiveSurfaceView(Operations operations);

  void Render(SDL_Renderer* renderer,
              const WorkspaceLayout& layout,
              bool draw_editor_caret,
              std::optional<SDL_FRect>* active_editor_pane_rect) const;

 private:
  Operations operations_;
};

class WorkspaceChromeView {
 public:
  using RenderFn = std::function<void(SDL_Renderer*, const WorkspaceLayout&)>;

  explicit WorkspaceChromeView(RenderFn render);

  void Render(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;

 private:
  RenderFn render_;
};

class WorkspaceSidebarView {
 public:
  using RenderFn = std::function<void(SDL_Renderer*, const WorkspaceLayout&)>;

  explicit WorkspaceSidebarView(RenderFn render);

  void Render(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;

 private:
  RenderFn render_;
};

class WorkspaceOverlayView {
 public:
  using RenderFn = std::function<void(SDL_Renderer*, const WorkspaceLayout&)>;

  explicit WorkspaceOverlayView(RenderFn render);

  void Render(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;

 private:
  RenderFn render_;
};

class WorkspacePanelView {
 public:
  struct Operations {
    std::function<std::size_t()> active_terminal_line_count;
    std::function<void(SDL_Renderer*, const WorkspaceLayout&, std::size_t)> render;
  };

  explicit WorkspacePanelView(Operations operations);

  void Render(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;

 private:
  Operations operations_;
};

class WorkspaceMenuView {
 public:
  using RenderFn = std::function<void(SDL_Renderer*, const WorkspaceLayout&)>;

  explicit WorkspaceMenuView(RenderFn render);

  void Render(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;

 private:
  RenderFn render_;
};

class WorkspacePromptView {
 public:
  using RenderFn = std::function<void(SDL_Renderer*,
                                      const WorkspaceLayout&,
                                      const std::optional<SDL_FRect>&)>;

  explicit WorkspacePromptView(RenderFn render);

  void Render(SDL_Renderer* renderer,
              const WorkspaceLayout& layout,
              const std::optional<SDL_FRect>& active_editor_pane_rect) const;

 private:
  RenderFn render_;
};

}  // namespace microide::workspace
