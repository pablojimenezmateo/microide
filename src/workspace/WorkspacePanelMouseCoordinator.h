#pragma once

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class WorkspaceShell::PanelMouseCoordinator {
 public:
  explicit PanelMouseCoordinator(WorkspaceShell& shell);

  bool HandleResizeButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleButtonUp(const SDL_Event& event);
  bool HandleDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleMotion(const SDL_Event& event);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks);

 private:
  bool HandleMouseCaptureButton(const SDL_Event& event, bool pressed);

  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
