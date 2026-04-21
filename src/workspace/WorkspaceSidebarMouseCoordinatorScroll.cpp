#include "workspace/WorkspaceSidebarMouseCoordinator.h"

#include <algorithm>
#include <cmath>

#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

bool SidebarMouseCoordinator::HandleDrag(const SDL_Event& event,
                                         const WorkspaceLayout& layout) {
  if (interaction_state_.drag_target != DragTarget::SidebarScrollbar || !state_.sidebar.visible) {
    return false;
  }

  const auto list_layout = CurrentListLayout(layout);
  if (!list_layout.scrollbar.has_value()) {
    interaction_state_.drag_target = DragTarget::None;
    return false;
  }

  state_.sidebar.scroll_row = std::clamp(
      static_cast<int>(std::lround(ScrollUnitsForPointer(
          *list_layout.scrollbar, static_cast<float>(event.motion.y),
          interaction_state_.drag_scrollbar_offset))),
      0, list_layout.max_scroll);
  state_.surface.focus = FocusTarget::Sidebar;
  return true;
}

bool SidebarMouseCoordinator::HandleWheel(const SDL_Event& event,
                                          const WorkspaceLayout& layout,
                                          int vertical_ticks) {
  if (!state_.sidebar.visible ||
      !Contains(layout.sidebar, event.wheel.mouse_x, event.wheel.mouse_y)) {
    return false;
  }

  const auto list_layout = CurrentListLayout(layout);
  state_.sidebar.scroll_row =
      std::clamp(state_.sidebar.scroll_row - vertical_ticks, 0, list_layout.max_scroll);
  state_.surface.focus = FocusTarget::Sidebar;
  return true;
}

bool SidebarMouseCoordinator::BeginScrollbarDrag(const SDL_Event& event,
                                                 const WorkspaceLayout& layout) {
  if (!state_.sidebar.visible) {
    return false;
  }

  const auto list_layout = CurrentListLayout(layout);
  if (!list_layout.scrollbar.has_value() ||
      !Contains(list_layout.scrollbar->track, event.button.x, event.button.y)) {
    return false;
  }

  interaction_state_.drag_target = DragTarget::SidebarScrollbar;
  interaction_state_.drag_scrollbar_offset =
      Contains(list_layout.scrollbar->thumb, event.button.x, event.button.y)
          ? static_cast<float>(event.button.y) - list_layout.scrollbar->thumb.y
          : list_layout.scrollbar->thumb.h * 0.5f;
  state_.sidebar.scroll_row = std::clamp(
      static_cast<int>(std::lround(ScrollUnitsForPointer(
          *list_layout.scrollbar, static_cast<float>(event.button.y),
          interaction_state_.drag_scrollbar_offset))),
      0, list_layout.max_scroll);
  state_.surface.focus = FocusTarget::Sidebar;
  return true;
}

ScrollableListLayout SidebarMouseCoordinator::CurrentListLayout(const WorkspaceLayout& layout) const {
  const SidebarMode sidebar_mode = operations_.active_sidebar_mode();
  if (sidebar_mode == SidebarMode::Search) {
    const auto line_map = operations_.build_project_search_line_map();
    return operations_.compute_project_search_sidebar_list_layout(layout.sidebar, line_map.size());
  }
  if (sidebar_mode == SidebarMode::Git) {
    const auto lines = operations_.build_git_sidebar_lines();
    return operations_.compute_git_sidebar_list_layout(layout.sidebar, lines.size());
  }
  if (sidebar_mode == SidebarMode::Problems) {
    return operations_.compute_problems_sidebar_list_layout(layout.sidebar,
                                                            state_.sidebar.problems.entries.size());
  }
  if (sidebar_mode == SidebarMode::Tests) {
    return operations_.compute_tests_sidebar_list_layout(layout.sidebar,
                                                         state_.sidebar.tests.entries.size());
  }
  if (sidebar_mode == SidebarMode::Plugin) {
    return operations_.compute_plugin_sidebar_list_layout(layout.sidebar,
                                                          state_.sidebar.plugin.items.size());
  }
  return operations_.compute_tree_sidebar_list_layout(layout.sidebar,
                                                      state_.directory_tree.entries().size());
}

}  // namespace microide::workspace
