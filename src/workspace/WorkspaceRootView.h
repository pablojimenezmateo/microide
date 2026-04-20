#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <optional>

#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceViewTree.h"

namespace microide::workspace {

class WorkspaceRootView {
 public:
  struct FrameOperations {
    std::function<void(SDL_Renderer*, int, int)> prepare_render_frame;
    std::function<WorkspaceLayout(int, int)> compute_layout;
    std::function<void()> reset_visible_editor_blame_overlay;
    std::function<bool()> should_draw_editor_caret;
    std::function<void(SDL_Renderer*, const WorkspaceLayout&)> render_frame_base;
  };

  struct Views {
    WorkspaceActiveSurfaceView active_surface;
    WorkspaceChromeView chrome;
    WorkspaceSidebarView sidebar;
    WorkspaceOverlayView overlay;
    WorkspacePanelView panel;
    WorkspaceMenuView menu;
    WorkspacePromptView prompt;
  };

  WorkspaceRootView(FrameOperations frame_operations, Views views);

  void Render(SDL_Renderer* renderer, int width, int height);
  void RenderPrepared(SDL_Renderer* renderer, int width, int height);

 private:
  FrameOperations frame_operations_;
  Views views_;
};

}  // namespace microide::workspace
