#include "workspace/WorkspaceSidebarMouseCoordinator.h"

#include <algorithm>
#include <cmath>

#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

bool SidebarMouseCoordinator::HandleDrag(const SDL_Event& event,
                                         const WorkspaceLayout& layout) {
  if (shell_.interaction_state_.drag_target != WorkspaceShell::DragTarget::SidebarScrollbar ||
      !shell_.sidebar_state_.visible) {
    return false;
  }

  const auto list_layout = CurrentListLayout(layout);
  if (!list_layout.scrollbar.has_value()) {
    shell_.ClearDragState();
    return false;
  }

  shell_.sidebar_state_.scroll_row = std::clamp(
      static_cast<int>(std::lround(ScrollUnitsForPointer(
          *list_layout.scrollbar, static_cast<float>(event.motion.y),
          shell_.interaction_state_.drag_scrollbar_offset))),
      0, list_layout.max_scroll);
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Sidebar;
  return true;
}

bool SidebarMouseCoordinator::HandleWheel(const SDL_Event& event,
                                          const WorkspaceLayout& layout,
                                          int vertical_ticks) {
  if (!shell_.sidebar_state_.visible ||
      !Contains(layout.sidebar, event.wheel.mouse_x, event.wheel.mouse_y)) {
    return false;
  }

  const auto list_layout = CurrentListLayout(layout);
  shell_.sidebar_state_.scroll_row =
      std::clamp(shell_.sidebar_state_.scroll_row - vertical_ticks, 0, list_layout.max_scroll);
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Sidebar;
  return true;
}

bool SidebarMouseCoordinator::BeginScrollbarDrag(const SDL_Event& event,
                                                 const WorkspaceLayout& layout) {
  if (!shell_.sidebar_state_.visible) {
    return false;
  }

  const auto list_layout = CurrentListLayout(layout);
  if (!list_layout.scrollbar.has_value() ||
      !Contains(list_layout.scrollbar->track, event.button.x, event.button.y)) {
    return false;
  }

  shell_.interaction_state_.drag_target = WorkspaceShell::DragTarget::SidebarScrollbar;
  shell_.interaction_state_.drag_scrollbar_offset =
      Contains(list_layout.scrollbar->thumb, event.button.x, event.button.y)
          ? static_cast<float>(event.button.y) - list_layout.scrollbar->thumb.y
          : list_layout.scrollbar->thumb.h * 0.5f;
  shell_.sidebar_state_.scroll_row = std::clamp(
      static_cast<int>(std::lround(ScrollUnitsForPointer(
          *list_layout.scrollbar, static_cast<float>(event.button.y),
          shell_.interaction_state_.drag_scrollbar_offset))),
      0, list_layout.max_scroll);
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Sidebar;
  return true;
}

ScrollableListLayout SidebarMouseCoordinator::CurrentListLayout(const WorkspaceLayout& layout) const {
  const SidebarMode sidebar_mode = shell_.ActiveSidebarMode();
  if (sidebar_mode == SidebarMode::Search) {
    const auto line_map = shell_.BuildProjectSearchLineMap();
    return shell_.ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
  }
  if (sidebar_mode == SidebarMode::Git) {
    const auto lines = shell_.BuildGitSidebarLines();
    return shell_.ComputeGitSidebarListLayout(layout.sidebar, lines.size());
  }
  if (sidebar_mode == SidebarMode::Problems) {
    return shell_.ComputeProblemsSidebarListLayout(layout.sidebar,
                                                   shell_.problems_sidebar_.entries.size());
  }
  if (sidebar_mode == SidebarMode::Plugin) {
    return shell_.ComputePluginSidebarListLayout(layout.sidebar,
                                                 shell_.plugin_sidebar_.items.size());
  }
  return shell_.ComputeTreeSidebarListLayout(layout.sidebar,
                                             shell_.directory_tree_.entries().size());
}

}  // namespace microide::workspace
