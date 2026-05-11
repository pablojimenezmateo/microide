#include "workspace/WorkspaceSidebarMouseCoordinator.h"

#include <utility>

#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

constexpr float kSidebarHeaderHeight = 26.0f;

}  // namespace

SidebarMouseCoordinator::SidebarMouseCoordinator(ProjectWorkspaceState& state,
                                                 InteractionState& interaction_state,
                                                 Operations operations)
    : state_(state), interaction_state_(interaction_state), operations_(std::move(operations)) {}

bool SidebarMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                               const WorkspaceLayout& layout) {
  if (event.button.button == SDL_BUTTON_LEFT && BeginScrollbarDrag(event, layout)) {
    return true;
  }

  if (!state_.sidebar.visible || !Contains(layout.sidebar, event.button.x, event.button.y)) {
    return false;
  }

  state_.surface.focus = FocusTarget::Sidebar;
  const float local_y = event.button.y - (layout.sidebar.y + kSidebarHeaderHeight + 6.0f);
  const SidebarMode sidebar_mode = operations_.active_sidebar_mode();

  if (sidebar_mode == SidebarMode::Search) {
    return HandleSearchButtonDown(event, layout, local_y);
  }
  if (sidebar_mode == SidebarMode::Git) {
    return HandleGitButtonDown(event, layout, local_y);
  }
  if (sidebar_mode == SidebarMode::Problems) {
    return HandleProblemsButtonDown(event, layout, local_y);
  }
  if (sidebar_mode == SidebarMode::Tests) {
    return HandleTestsButtonDown(event, layout, local_y);
  }
  if (sidebar_mode == SidebarMode::Plugin) {
    return HandlePluginButtonDown(event, layout, local_y);
  }
  if (sidebar_mode == SidebarMode::Outline) {
    return HandleOutlineButtonDown(event, layout, local_y);
  }
  return HandleTreeButtonDown(event, layout, local_y);
}

bool SidebarMouseCoordinator::HandleSearchButtonDown(const SDL_Event& event,
                                                     const WorkspaceLayout& layout,
                                                     float local_y) {
  if (event.button.button != SDL_BUTTON_LEFT) {
    return true;
  }
  if (Contains(operations_.project_search_query_rect(layout.sidebar), event.button.x, event.button.y)) {
    operations_.begin_project_search_edit(ProjectSearchEditField::Query);
    return true;
  }
  if (Contains(operations_.project_search_replace_rect(layout.sidebar), event.button.x, event.button.y)) {
    operations_.begin_project_search_edit(ProjectSearchEditField::Replace);
    return true;
  }
  if (Contains(operations_.project_search_mode_button_rect(layout.sidebar), event.button.x,
               event.button.y)) {
    if (state_.overlay.workflow.project_search.editing) {
      operations_.commit_project_search_edit();
    }
    operations_.toggle_project_search_pattern_mode();
    return true;
  }
  if (Contains(operations_.project_search_case_button_rect(layout.sidebar), event.button.x,
               event.button.y)) {
    if (state_.overlay.workflow.project_search.editing) {
      operations_.commit_project_search_edit();
    }
    operations_.cycle_project_search_case_mode();
    return true;
  }
  if (Contains(operations_.project_search_hidden_button_rect(layout.sidebar), event.button.x,
               event.button.y)) {
    if (state_.overlay.workflow.project_search.editing) {
      operations_.commit_project_search_edit();
    }
    operations_.toggle_project_search_hidden_files();
    return true;
  }
  if (local_y < 0.0f) {
    return true;
  }

  const auto line_map = operations_.build_project_search_line_map();
  const auto list_layout =
      operations_.compute_project_search_sidebar_list_layout(layout.sidebar, line_map.size());
  if (const auto line_index =
          ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
      line_index.has_value() && *line_index >= 0 &&
      *line_index < static_cast<int>(line_map.size()) &&
      line_map[static_cast<std::size_t>(*line_index)] >= 0) {
    state_.overlay.workflow.project_search.selected_index =
        static_cast<std::size_t>(line_map[static_cast<std::size_t>(*line_index)]);
    const auto& result =
        state_.overlay.workflow.project_search.results[state_.overlay.workflow.project_search.selected_index];
    operations_.open_file_at_location(state_.root / result.relative_path, result.line, result.column);
    if (state_.sidebar.temporary) {
      operations_.restore_previous_sidebar();
    }
    state_.surface.focus = FocusTarget::Editor;
  }
  return true;
}

bool SidebarMouseCoordinator::HandleGitButtonDown(const SDL_Event& event,
                                                  const WorkspaceLayout& layout,
                                                  float local_y) {
  if (event.button.button != SDL_BUTTON_LEFT) {
    return true;
  }
  if (operations_.can_stage_all_git_sidebar_entries() &&
      Contains(operations_.git_sidebar_stage_all_button_rect(layout.sidebar), event.button.x,
               event.button.y)) {
    return operations_.stage_all_git_sidebar_entries();
  }
  if (operations_.can_discard_all_git_sidebar_entries() &&
      Contains(operations_.git_sidebar_discard_all_button_rect(layout.sidebar), event.button.x,
               event.button.y)) {
    operations_.open_discard_all_git_sidebar_prompt();
    return true;
  }
  if (Contains(operations_.git_sidebar_refresh_button_rect(layout.sidebar), event.button.x,
               event.button.y)) {
    return operations_.execute_action(ActionId::GitRefresh, {}, ActionSource::Shortcut);
  }
  if (const auto button_rect = operations_.git_sidebar_outgoing_base_button_rect(layout.sidebar);
      button_rect.has_value() && Contains(*button_rect, event.button.x, event.button.y)) {
    operations_.open_anchored_menu(MenuId::GitOutgoingBase, *button_rect);
    return true;
  }
  if (event.button.y < operations_.git_sidebar_list_top(layout.sidebar) || local_y < 0.0f) {
    return true;
  }

  const auto lines = operations_.build_git_sidebar_lines();
  const auto list_layout = operations_.compute_git_sidebar_list_layout(layout.sidebar, lines.size());
  const auto line_index = ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
  if (!line_index.has_value() || *line_index < 0 || *line_index >= static_cast<int>(lines.size())) {
    return true;
  }

  const auto& line = lines[static_cast<std::size_t>(*line_index)];
  if (line.kind != GitSidebarLine::Kind::Entry || line.entry_index < 0) {
    return true;
  }

  state_.sidebar.git.selected_index = static_cast<std::size_t>(line.entry_index);
  const auto& entry = state_.sidebar.git.entries[state_.sidebar.git.selected_index];
  const SDL_FRect row_rect = ScrollableListRowRect(list_layout, *line_index - list_layout.scroll_row);
  const GitSidebarEntryActionLayout actions =
      operations_.compute_git_sidebar_entry_action_layout(row_rect, entry);
  if (actions.primary_rect.has_value() &&
      Contains(*actions.primary_rect, event.button.x, event.button.y)) {
    return entry.staged ? operations_.unstage_git_sidebar_entry(state_.sidebar.git.selected_index)
                        : operations_.stage_git_sidebar_entry(state_.sidebar.git.selected_index);
  }
  if (actions.discard_rect.has_value() &&
      Contains(*actions.discard_rect, event.button.x, event.button.y)) {
    operations_.discard_git_sidebar_entry(state_.sidebar.git.selected_index);
    return true;
  }
  operations_.open_git_sidebar_entry(state_.sidebar.git.selected_index);
  return true;
}

bool SidebarMouseCoordinator::HandleProblemsButtonDown(const SDL_Event& event,
                                                       const WorkspaceLayout& layout,
                                                       float local_y) {
  if (local_y < 0.0f) {
    return true;
  }

  const auto list_layout =
      operations_.compute_problems_sidebar_list_layout(layout.sidebar, state_.sidebar.problems.entries.size());
  const auto item_index = ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
  if (!item_index.has_value() || *item_index < 0 ||
      *item_index >= static_cast<int>(state_.sidebar.problems.entries.size())) {
    return true;
  }

  state_.sidebar.problems.selected_index = static_cast<std::size_t>(*item_index);
  operations_.reveal_selected_problems_sidebar_line();
  if (event.button.button == SDL_BUTTON_LEFT) {
    operations_.open_selected_problem_sidebar_item();
  }
  return true;
}

bool SidebarMouseCoordinator::HandleTestsButtonDown(const SDL_Event& event,
                                                    const WorkspaceLayout& layout,
                                                    float local_y) {
  if (local_y < 0.0f) {
    return true;
  }

  const auto list_layout =
      operations_.compute_tests_sidebar_list_layout(layout.sidebar, state_.sidebar.tests.entries.size());
  const auto item_index = ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
  if (!item_index.has_value() || *item_index < 0 ||
      *item_index >= static_cast<int>(state_.sidebar.tests.entries.size())) {
    return true;
  }

  state_.sidebar.tests.selected_index = static_cast<std::size_t>(*item_index);
  operations_.reveal_selected_tests_sidebar_line();
  if (event.button.button == SDL_BUTTON_LEFT) {
    operations_.open_selected_test_sidebar_item();
  } else if (event.button.button == SDL_BUTTON_MIDDLE) {
    operations_.run_selected_test_sidebar_item();
  }
  return true;
}

bool SidebarMouseCoordinator::HandlePluginButtonDown(const SDL_Event& event,
                                                     const WorkspaceLayout& layout,
                                                     float local_y) {
  if (local_y < 0.0f) {
    return true;
  }

  const auto list_layout =
      operations_.compute_plugin_sidebar_list_layout(layout.sidebar, state_.sidebar.plugin.items.size());
  const auto item_index = ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
  if (!item_index.has_value() || *item_index < 0 ||
      *item_index >= static_cast<int>(state_.sidebar.plugin.items.size())) {
    return true;
  }

  state_.sidebar.plugin.selected_index = static_cast<std::size_t>(*item_index);
  operations_.reveal_selected_plugin_sidebar_line();
  if (event.button.button == SDL_BUTTON_LEFT) {
    operations_.open_selected_plugin_sidebar_item();
  }
  return true;
}

bool SidebarMouseCoordinator::HandleOutlineButtonDown(const SDL_Event& event,
                                                       const WorkspaceLayout& layout,
                                                       float local_y) {
  if (local_y < 0.0f) {
    return true;
  }
  if (operations_.handle_outline_sidebar_pointer_down) {
    operations_.handle_outline_sidebar_pointer_down(event, layout);
  }
  return true;
}

bool SidebarMouseCoordinator::HandleTreeButtonDown(const SDL_Event& event,
                                                   const WorkspaceLayout& layout,
                                                   float local_y) {
  if (event.button.button == SDL_BUTTON_LEFT && operations_.can_collapse_tree() &&
      Contains(operations_.tree_sidebar_collapse_button_rect(layout.sidebar), event.button.x,
               event.button.y)) {
    operations_.collapse_all_tree();
    operations_.reveal_selected_tree_sidebar_line();
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT &&
      Contains(operations_.tree_sidebar_refresh_button_rect(layout.sidebar), event.button.x,
               event.button.y)) {
    return operations_.execute_action(ActionId::TreeRefresh, {}, ActionSource::Shortcut);
  }

  if (local_y < 0.0f) {
    if (event.button.button == SDL_BUTTON_RIGHT) {
      operations_.open_tree_context_menu(
          TreeContextTargetKind::Background, {},
          MakeRect(static_cast<float>(event.button.x), static_cast<float>(event.button.y), 1.0f,
                   1.0f));
    }
    return true;
  }

  const auto& entries = state_.directory_tree.entries();
  const auto list_layout = operations_.compute_tree_sidebar_list_layout(layout.sidebar, entries.size());
  const auto entry_index = ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
  if (entry_index.has_value() && *entry_index >= 0 &&
      *entry_index < static_cast<int>(entries.size())) {
    state_.directory_tree.SetSelectedIndex(static_cast<std::size_t>(*entry_index));
    const SDL_FRect row_rect = ScrollableListRowRect(list_layout, *entry_index - list_layout.scroll_row);
    if (Contains(row_rect, event.button.x, event.button.y) &&
        event.button.button == SDL_BUTTON_RIGHT) {
      const auto& entry = entries[static_cast<std::size_t>(*entry_index)];
      const TreeContextTargetKind target =
          !entry.is_directory ? TreeContextTargetKind::File
          : entry.path == state_.root ? TreeContextTargetKind::Root
                                      : TreeContextTargetKind::Directory;
      operations_.open_tree_context_menu(
          target, entry.path,
          MakeRect(static_cast<float>(event.button.x), static_cast<float>(event.button.y), 1.0f,
                   1.0f));
      return true;
    }
    if (Contains(row_rect, event.button.x, event.button.y) &&
        event.button.button != SDL_BUTTON_RIGHT) {
      const auto opened = state_.directory_tree.ActivateSelection();
      operations_.reveal_selected_tree_sidebar_line();
      if (opened.has_value()) {
        operations_.open_file(*opened);
      }
    }
    return true;
  }

  if (event.button.button == SDL_BUTTON_RIGHT) {
    operations_.open_tree_context_menu(
        TreeContextTargetKind::Background, {},
        MakeRect(static_cast<float>(event.button.x), static_cast<float>(event.button.y), 1.0f,
                 1.0f));
  }
  return true;
}

}  // namespace microide::workspace
