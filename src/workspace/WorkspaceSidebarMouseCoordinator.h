#pragma once

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class WorkspaceShell::SidebarMouseCoordinator {
 public:
  explicit SidebarMouseCoordinator(WorkspaceShell& shell);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks);

 private:
  bool BeginScrollbarDrag(const SDL_Event& event, const WorkspaceLayout& layout);

  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
