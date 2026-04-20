#pragma once

#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

class SidebarMouseCoordinator {
 public:
  explicit SidebarMouseCoordinator(WorkspaceShell& shell);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks);

 private:
  bool HandleSearchButtonDown(const SDL_Event& event,
                              const WorkspaceLayout& layout,
                              float local_y);
  bool HandleGitButtonDown(const SDL_Event& event,
                           const WorkspaceLayout& layout,
                           float local_y);
  bool HandleProblemsButtonDown(const SDL_Event& event,
                                const WorkspaceLayout& layout,
                                float local_y);
  bool HandlePluginButtonDown(const SDL_Event& event,
                              const WorkspaceLayout& layout,
                              float local_y);
  bool HandleTreeButtonDown(const SDL_Event& event,
                            const WorkspaceLayout& layout,
                            float local_y);
  bool BeginScrollbarDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  ScrollableListLayout CurrentListLayout(const WorkspaceLayout& layout) const;

  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
