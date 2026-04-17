#include "workspace/WorkspaceSidebarMouseCoordinator.h"

#include <algorithm>
#include <cmath>

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr float kSidebarHeaderHeight = 26.0f;

}  // namespace

WorkspaceShell::SidebarMouseCoordinator::SidebarMouseCoordinator(WorkspaceShell& shell)
    : shell_(shell) {}

bool WorkspaceShell::SidebarMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                                               const WorkspaceLayout& layout) {
  if (event.button.button == SDL_BUTTON_LEFT && BeginScrollbarDrag(event, layout)) {
    return true;
  }

  if (!shell_.surface_.sidebar_visible ||
      !Contains(layout.sidebar, event.button.x, event.button.y)) {
    return false;
  }

  shell_.surface_.focus = FocusTarget::Sidebar;
  const float local_y =
      event.button.y - (layout.sidebar.y + kSidebarHeaderHeight + 6.0f);

  if (shell_.surface_.sidebar_mode == SidebarMode::Search) {
    if (event.button.button != SDL_BUTTON_LEFT) {
      return true;
    }
    if (Contains(shell_.ProjectSearchQueryRect(layout.sidebar), event.button.x,
                 event.button.y)) {
      shell_.BeginProjectSearchEdit(ProjectSearchEditField::Query);
      return true;
    }
    if (Contains(shell_.ProjectSearchReplaceRect(layout.sidebar), event.button.x,
                 event.button.y)) {
      shell_.BeginProjectSearchEdit(ProjectSearchEditField::Replace);
      return true;
    }
    if (Contains(shell_.ProjectSearchModeButtonRect(layout.sidebar), event.button.x,
                 event.button.y)) {
      if (shell_.overlay_workflow_.project_search.editing) {
        shell_.CommitProjectSearchEdit();
      }
      shell_.ToggleProjectSearchPatternMode();
      return true;
    }
    if (Contains(shell_.ProjectSearchCaseButtonRect(layout.sidebar), event.button.x,
                 event.button.y)) {
      if (shell_.overlay_workflow_.project_search.editing) {
        shell_.CommitProjectSearchEdit();
      }
      shell_.CycleProjectSearchCaseMode();
      return true;
    }
    if (Contains(shell_.ProjectSearchHiddenButtonRect(layout.sidebar), event.button.x,
                 event.button.y)) {
      if (shell_.overlay_workflow_.project_search.editing) {
        shell_.CommitProjectSearchEdit();
      }
      shell_.ToggleProjectSearchHiddenFiles();
      return true;
    }
    if (local_y < 0.0f) {
      return true;
    }

    const auto line_map = shell_.BuildProjectSearchLineMap();
    const auto list_layout =
        shell_.ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
    if (const auto line_index =
            ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
        line_index.has_value() && *line_index >= 0 &&
        *line_index < static_cast<int>(line_map.size()) &&
        line_map[static_cast<std::size_t>(*line_index)] >= 0) {
      shell_.overlay_workflow_.project_search.selected_index =
          static_cast<std::size_t>(line_map[static_cast<std::size_t>(*line_index)]);
      const auto& result = shell_.overlay_workflow_.project_search
                               .results[shell_.overlay_workflow_.project_search.selected_index];
      shell_.OpenFile(shell_.project_root_ / result.relative_path);
      shell_.text_viewport_.MoveCursorTo(result.line, result.column);
      if (shell_.surface_.sidebar_temporary) {
        shell_.RestorePreviousSidebar();
      }
      shell_.surface_.focus = FocusTarget::Editor;
    }
    return true;
  }

  if (shell_.surface_.sidebar_mode == SidebarMode::Git) {
    if (event.button.button != SDL_BUTTON_LEFT) {
      return true;
    }
    if (shell_.CanStageAllGitSidebarEntries() &&
        Contains(shell_.GitSidebarStageAllButtonRect(layout.sidebar), event.button.x,
                 event.button.y)) {
      return shell_.StageAllGitSidebarEntries();
    }
    if (shell_.CanDiscardAllGitSidebarEntries() &&
        Contains(shell_.GitSidebarDiscardAllButtonRect(layout.sidebar), event.button.x,
                 event.button.y)) {
      shell_.OpenDiscardAllGitSidebarPrompt();
      return true;
    }
    if (Contains(shell_.GitSidebarRefreshButtonRect(layout.sidebar), event.button.x,
                 event.button.y)) {
      return ActionCoordinator(shell_).Execute(ActionId::GitRefresh, {}, ActionSource::Shortcut);
    }
    if (event.button.y < shell_.GitSidebarListTop(layout.sidebar)) {
      return true;
    }

    const auto lines = shell_.BuildGitSidebarLines();
    const auto list_layout =
        shell_.ComputeGitSidebarListLayout(layout.sidebar, lines.size());
    const auto line_index =
        ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
    if (!line_index.has_value() || *line_index < 0 ||
        *line_index >= static_cast<int>(lines.size())) {
      return true;
    }

    const auto& line = lines[static_cast<std::size_t>(*line_index)];
    if (line.kind != GitSidebarLine::Kind::Entry || line.entry_index < 0) {
      return true;
    }

    shell_.git_sidebar_.selected_index = static_cast<std::size_t>(line.entry_index);
    const auto& entry = shell_.git_sidebar_.entries[shell_.git_sidebar_.selected_index];
    const SDL_FRect row_rect =
        ScrollableListRowRect(list_layout, *line_index - list_layout.scroll_row);
    const GitSidebarEntryActionLayout actions =
        shell_.ComputeGitSidebarEntryActionLayout(row_rect, entry);
    if (actions.primary_rect.has_value() &&
        Contains(*actions.primary_rect, event.button.x, event.button.y)) {
      if (entry.staged) {
        shell_.UnstageGitSidebarEntry(shell_.git_sidebar_.selected_index);
      } else {
        shell_.StageGitSidebarEntry(shell_.git_sidebar_.selected_index);
      }
      return true;
    }
    if (actions.discard_rect.has_value() &&
        Contains(*actions.discard_rect, event.button.x, event.button.y)) {
      shell_.DiscardGitSidebarEntry(shell_.git_sidebar_.selected_index);
      return true;
    }
    shell_.OpenGitSidebarEntry(shell_.git_sidebar_.selected_index);
    return true;
  }

  if (shell_.surface_.sidebar_mode == SidebarMode::Problems) {
    if (local_y < 0.0f) {
      return true;
    }

    const auto list_layout = shell_.ComputeProblemsSidebarListLayout(
        layout.sidebar, shell_.problems_sidebar_.entries.size());
    const auto item_index =
        ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
    if (!item_index.has_value() || *item_index < 0 ||
        *item_index >= static_cast<int>(shell_.problems_sidebar_.entries.size())) {
      return true;
    }

    shell_.problems_sidebar_.selected_index = static_cast<std::size_t>(*item_index);
    shell_.RevealSelectedProblemsSidebarLine();
    if (event.button.button == SDL_BUTTON_LEFT) {
      shell_.OpenSelectedProblemSidebarItem();
    }
    return true;
  }

  if (shell_.surface_.sidebar_mode == SidebarMode::Plugin) {
    if (local_y < 0.0f) {
      return true;
    }

    const auto list_layout =
        shell_.ComputePluginSidebarListLayout(layout.sidebar, shell_.plugin_sidebar_.items.size());
    const auto item_index =
        ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
    if (!item_index.has_value() || *item_index < 0 ||
        *item_index >= static_cast<int>(shell_.plugin_sidebar_.items.size())) {
      return true;
    }

    shell_.plugin_sidebar_.selected_index = static_cast<std::size_t>(*item_index);
    shell_.RevealSelectedPluginSidebarLine();
    if (event.button.button == SDL_BUTTON_LEFT) {
      shell_.OpenSelectedPluginSidebarItem();
    }
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && shell_.directory_tree_.CanCollapseAll() &&
      Contains(shell_.TreeSidebarCollapseButtonRect(layout.sidebar), event.button.x,
               event.button.y)) {
    shell_.directory_tree_.CollapseAll();
    shell_.RevealSelectedTreeSidebarLine();
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT &&
      Contains(shell_.TreeSidebarRefreshButtonRect(layout.sidebar), event.button.x,
               event.button.y)) {
    return ActionCoordinator(shell_).Execute(ActionId::TreeRefresh, {}, ActionSource::Shortcut);
  }

  if (local_y < 0.0f) {
    if (event.button.button == SDL_BUTTON_RIGHT) {
      shell_.OpenTreeContextMenu(TreeContextTargetKind::Background, {},
                                 MakeRect(static_cast<float>(event.button.x),
                                          static_cast<float>(event.button.y), 1.0f, 1.0f));
    }
    return true;
  }

  const auto& entries = shell_.directory_tree_.entries();
  const auto list_layout =
      shell_.ComputeTreeSidebarListLayout(layout.sidebar, entries.size());
  const auto entry_index =
      ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
  if (entry_index.has_value() && *entry_index >= 0 &&
      *entry_index < static_cast<int>(entries.size())) {
    shell_.directory_tree_.SetSelectedIndex(static_cast<std::size_t>(*entry_index));
    const SDL_FRect row_rect =
        ScrollableListRowRect(list_layout, *entry_index - list_layout.scroll_row);
    if (Contains(row_rect, event.button.x, event.button.y) &&
        event.button.button == SDL_BUTTON_RIGHT) {
      const auto& entry = entries[static_cast<std::size_t>(*entry_index)];
      const TreeContextTargetKind target =
          !entry.is_directory ? TreeContextTargetKind::File
          : entry.path == shell_.project_root_ ? TreeContextTargetKind::Root
                                               : TreeContextTargetKind::Directory;
      shell_.OpenTreeContextMenu(target, entry.path,
                                 MakeRect(static_cast<float>(event.button.x),
                                          static_cast<float>(event.button.y), 1.0f, 1.0f));
      return true;
    }
    if (Contains(row_rect, event.button.x, event.button.y) &&
        event.button.button != SDL_BUTTON_RIGHT) {
      const auto opened = shell_.directory_tree_.ActivateSelection();
      shell_.RevealSelectedTreeSidebarLine();
      if (opened.has_value()) {
        shell_.OpenFile(*opened);
      }
    }
    return true;
  }

  if (event.button.button == SDL_BUTTON_RIGHT) {
    shell_.OpenTreeContextMenu(TreeContextTargetKind::Background, {},
                               MakeRect(static_cast<float>(event.button.x),
                                        static_cast<float>(event.button.y), 1.0f, 1.0f));
  }
  return true;
}

bool WorkspaceShell::SidebarMouseCoordinator::HandleDrag(const SDL_Event& event,
                                                         const WorkspaceLayout& layout) {
  if (shell_.surface_.drag_target != DragTarget::SidebarScrollbar ||
      !shell_.surface_.sidebar_visible) {
    return false;
  }

  if (shell_.surface_.sidebar_mode == SidebarMode::Search) {
    const auto line_map = shell_.BuildProjectSearchLineMap();
    const auto list_layout =
        shell_.ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
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
  } else if (shell_.surface_.sidebar_mode == SidebarMode::Git) {
    const auto lines = shell_.BuildGitSidebarLines();
    const auto list_layout =
        shell_.ComputeGitSidebarListLayout(layout.sidebar, lines.size());
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
  } else if (shell_.surface_.sidebar_mode == SidebarMode::Problems) {
    const auto list_layout = shell_.ComputeProblemsSidebarListLayout(
        layout.sidebar, shell_.problems_sidebar_.entries.size());
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
  } else if (shell_.surface_.sidebar_mode == SidebarMode::Plugin) {
    const auto list_layout = shell_.ComputePluginSidebarListLayout(
        layout.sidebar, shell_.plugin_sidebar_.items.size());
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
  } else {
    const auto& entries = shell_.directory_tree_.entries();
    const auto list_layout =
        shell_.ComputeTreeSidebarListLayout(layout.sidebar, entries.size());
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
  }

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

  int max_scroll = 0;
  if (shell_.surface_.sidebar_mode == SidebarMode::Search) {
    const auto line_map = shell_.BuildProjectSearchLineMap();
    max_scroll =
        shell_.ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size())
            .max_scroll;
  } else if (shell_.surface_.sidebar_mode == SidebarMode::Git) {
    const auto lines = shell_.BuildGitSidebarLines();
    max_scroll =
        shell_.ComputeGitSidebarListLayout(layout.sidebar, lines.size()).max_scroll;
  } else if (shell_.surface_.sidebar_mode == SidebarMode::Problems) {
    max_scroll = shell_.ComputeProblemsSidebarListLayout(layout.sidebar,
                                                         shell_.problems_sidebar_.entries.size())
                     .max_scroll;
  } else if (shell_.surface_.sidebar_mode == SidebarMode::Plugin) {
    max_scroll = shell_.ComputePluginSidebarListLayout(layout.sidebar,
                                                       shell_.plugin_sidebar_.items.size())
                     .max_scroll;
  } else {
    const auto& entries = shell_.directory_tree_.entries();
    max_scroll =
        shell_.ComputeTreeSidebarListLayout(layout.sidebar, entries.size()).max_scroll;
  }

  shell_.surface_.sidebar_scroll_row =
      std::clamp(shell_.surface_.sidebar_scroll_row - vertical_ticks, 0, max_scroll);
  shell_.surface_.focus = FocusTarget::Sidebar;
  return true;
}

bool WorkspaceShell::SidebarMouseCoordinator::BeginScrollbarDrag(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  if (!shell_.surface_.sidebar_visible) {
    return false;
  }

  auto begin_drag = [&](const ScrollableListLayout& list_layout) {
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
  };

  if (shell_.surface_.sidebar_mode == SidebarMode::Search) {
    const auto line_map = shell_.BuildProjectSearchLineMap();
    return begin_drag(
        shell_.ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size()));
  }
  if (shell_.surface_.sidebar_mode == SidebarMode::Git) {
    const auto lines = shell_.BuildGitSidebarLines();
    return begin_drag(shell_.ComputeGitSidebarListLayout(layout.sidebar, lines.size()));
  }
  if (shell_.surface_.sidebar_mode == SidebarMode::Problems) {
    return begin_drag(shell_.ComputeProblemsSidebarListLayout(
        layout.sidebar, shell_.problems_sidebar_.entries.size()));
  }
  if (shell_.surface_.sidebar_mode == SidebarMode::Plugin) {
    return begin_drag(shell_.ComputePluginSidebarListLayout(layout.sidebar,
                                                            shell_.plugin_sidebar_.items.size()));
  }
  return begin_drag(
      shell_.ComputeTreeSidebarListLayout(layout.sidebar, shell_.directory_tree_.entries().size()));
}

}  // namespace microide::workspace
