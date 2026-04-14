#pragma once

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class WorkspaceShell::EditorMouseCoordinator {
 public:
  explicit EditorMouseCoordinator(WorkspaceShell& shell);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleSelectionMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks);

 private:
  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
