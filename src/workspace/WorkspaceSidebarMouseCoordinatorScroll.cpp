#include "workspace/WorkspaceSidebarMouseCoordinator.h"

#include <algorithm>
#include <cmath>

#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

bool WorkspaceShell::SidebarMouseCoordinator::HandleDrag(const SDL_Event& event,
                                                         const WorkspaceLayout& layout) {
  if (shell_.surface_.drag_target != DragTarget::SidebarScrollbar ||
      !shell_.surface_.sidebar_visible) {
    return false;
  }

  const auto list_layout = CurrentListLayout(layout);
  if (!list_layout.scrollbar.has_value()) {
    shell_.surface_.drag_target = DragTarget::None;
    shell_.surface_.drag_scrollbar_offset = 0.0f;
    return false;
  }

  shell_.surface_.sidebar_scroll_row = std::clamp(
      static_cast<int>(std::lround(ScrollUnitsForPointer(
          *list_layout.scrollbar, static_cast<float>(event.motion.y),
          shell_.surface_.drag_scrollbar_offset))),
      0, list_layout.max_scroll);
  shell_.surface_.focus = FocusTarget::Sidebar;
  return true;
}

bool WorkspaceShell::SidebarMouseCoordinator::HandleWheel(const SDL_Event& event,
                                                          const WorkspaceLayout& layout,
                                                          int vertical_ticks) {
  if (!shell_.surface_.sidebar_visible ||
      !Contains(layout.sidebar, event.wheel.mouse_x, event.wheel.mouse_y)) {
    return false;
  }

  const auto list_layout = CurrentListLayout(layout);
  shell_.surface_.sidebar_scroll_row =
      std::clamp(shell_.surface_.sidebar_scroll_row - vertical_ticks, 0, list_layout.max_scroll);
  shell_.surface_.focus = FocusTarget::Sidebar;
  return true;
}

bool WorkspaceShell::SidebarMouseCoordinator::BeginScrollbarDrag(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  if (!shell_.surface_.sidebar_visible) {
    return false;
  }

  const auto list_layout = CurrentListLayout(layout);
  if (!list_layout.scrollbar.has_value() ||
      !Contains(list_layout.scrollbar->track, event.button.x, event.button.y)) {
    return false;
  }

  shell_.surface_.drag_target = DragTarget::SidebarScrollbar;
  shell_.surface_.drag_scrollbar_offset =
      Contains(list_layout.scrollbar->thumb, event.button.x, event.button.y)
          ? static_cast<float>(event.button.y) - list_layout.scrollbar->thumb.y
          : list_layout.scrollbar->thumb.h * 0.5f;
  shell_.surface_.sidebar_scroll_row = std::clamp(
      static_cast<int>(std::lround(ScrollUnitsForPointer(
          *list_layout.scrollbar, static_cast<float>(event.button.y),
          shell_.surface_.drag_scrollbar_offset))),
      0, list_layout.max_scroll);
  shell_.surface_.focus = FocusTarget::Sidebar;
  return true;
}

ScrollableListLayout WorkspaceShell::SidebarMouseCoordinator::CurrentListLayout(
    const WorkspaceLayout& layout) const {
  if (shell_.surface_.sidebar_mode == SidebarMode::Search) {
    const auto line_map = shell_.BuildProjectSearchLineMap();
    return shell_.ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
  }
  if (shell_.surface_.sidebar_mode == SidebarMode::Git) {
    const auto lines = shell_.BuildGitSidebarLines();
    return shell_.ComputeGitSidebarListLayout(layout.sidebar, lines.size());
  }
  if (shell_.surface_.sidebar_mode == SidebarMode::Problems) {
    return shell_.ComputeProblemsSidebarListLayout(layout.sidebar,
                                                   shell_.problems_sidebar_.entries.size());
  }
  if (shell_.surface_.sidebar_mode == SidebarMode::Plugin) {
    return shell_.ComputePluginSidebarListLayout(layout.sidebar,
                                                 shell_.plugin_sidebar_.items.size());
  }
  return shell_.ComputeTreeSidebarListLayout(layout.sidebar,
                                             shell_.directory_tree_.entries().size());
}

}  // namespace microide::workspace
