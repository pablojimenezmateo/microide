#pragma once

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class ChromeMouseCoordinator {
 public:
  explicit ChromeMouseCoordinator(WorkspaceShell& shell);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks,
                   int horizontal_ticks);

 private:
  bool HandleMenuButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleMenuMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleOverlayButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleTreeContextMenuButtonDown(const SDL_Event& event);
  bool HandleTreeContextMenuMotion(const SDL_Event& event);

  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
