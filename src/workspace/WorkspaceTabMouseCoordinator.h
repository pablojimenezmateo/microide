#pragma once

#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class WorkspaceShell::TabMouseCoordinator {
 public:
  explicit TabMouseCoordinator(WorkspaceShell& shell);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleButtonUp(const SDL_Event& event);
  bool HandleMotion(const SDL_Event& event);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks);

 private:
  void PersistReorderedTabs(TabDragKind kind);

  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
