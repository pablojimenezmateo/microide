#include "workspace/coordinators/WorkspaceSidebarMouseCoordinator.h"

#include <algorithm>

#include "workspace/SidebarMetrics.h"

#include <utility>

#include "workspace/git/GitSidebarHeaderLayout.h"
#include "workspace/ProjectSearchPanelLayout.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

SDL_FRect PointerAnchor(const SDL_Event& event) {
  return MakeRect(static_cast<float>(event.button.x), static_cast<float>(event.button.y), 1.0f,
                  1.0f);
}

// A keyboard-opened menu hangs under the row it is about, a little in from its
// left edge, where a right-click on the row's text would have put it. A row
// scrolled out of view anchors at the list's top.
SDL_FRect RowAnchor(const ScrollableListLayout& list, int row) {
  const int visible = std::max(0, row - list.scroll_row);
  const SDL_FRect rect = ScrollableListRowRect(list, visible);
  return MakeRect(rect.x + 16.0f, rect.y + rect.h, 1.0f, 1.0f);
}

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
  // The layout's own origin (SidebarMetrics.h), not a second copy of it: this
  // used to re-derive `sidebar.y + 26 + 6` here, so the two had to be kept in
  // step by hand or every click would land on the wrong row.
  const float local_y = event.button.y - SidebarListTop(layout.sidebar);
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
  if (UsesPluginItemSurface(sidebar_mode)) {
    return HandlePluginButtonDown(event, layout, local_y);
  }
  return HandleTreeButtonDown(event, layout, local_y);
}

bool SidebarMouseCoordinator::HandleSearchButtonDown(const SDL_Event& event,
                                                     const WorkspaceLayout& layout,
                                                     float local_y) {
  const bool is_right = event.button.button == SDL_BUTTON_RIGHT;
  if (event.button.button != SDL_BUTTON_LEFT && !is_right) {
    return true;
  }
  // The header chrome (fields, toggles) is left-click only; the right button only
  // means anything over a result row.
  if (is_right) {
    return HandleSearchResultContextMenu(event, layout, local_y);
  }
  const SDL_FPoint point{static_cast<float>(event.button.x), static_cast<float>(event.button.y)};
  for (const auto& field : project_search_panel::SidebarSearchFieldRects(
           layout.sidebar, state_.overlay.workflow.project_search.scope_expanded)) {
    if (field.rect.w > 0.0f && Contains(field.rect, point.x, point.y)) {
      operations_.begin_project_search_edit(field.field);
      return true;
    }
  }

  // Every toggle button commits an in-flight field edit first so the click never
  // silently discards typed text.
  const struct {
    SDL_FRect rect;
    const std::function<void()>& action;
  } buttons[] = {
      {project_search_panel::ModeButtonRect(layout.sidebar),
       operations_.toggle_project_search_pattern_mode},
      {project_search_panel::CaseButtonRect(layout.sidebar),
       operations_.cycle_project_search_case_mode},
      {project_search_panel::HiddenButtonRect(layout.sidebar),
       operations_.toggle_project_search_hidden_files},
      {project_search_panel::ScopeButtonRect(layout.sidebar),
       operations_.toggle_project_search_scope_expanded},
      {project_search_panel::ReplaceAllButtonRect(layout.sidebar),
       operations_.request_project_search_replace_all},
  };
  for (const auto& button : buttons) {
    if (Contains(button.rect, point.x, point.y)) {
      if (state_.overlay.workflow.project_search.editing) {
        operations_.commit_project_search_edit();
      }
      button.action();
      return true;
    }
  }
  if (local_y < 0.0f) {
    return true;
  }

  const auto line_map = operations_.build_project_search_line_map();
  const auto list_layout =
      operations_.compute_project_search_sidebar_list_layout(layout.sidebar, line_map.size());
  if (const auto line_index =
          ScrollableListItemAtY(list_layout, static_cast<float>(event.button.y), line_map.size());
      line_index.has_value() && line_map[*line_index] >= 0) {
    state_.overlay.workflow.project_search.selected_index =
        static_cast<std::size_t>(line_map[*line_index]);
    const auto& result =
        state_.overlay.workflow.project_search.results[state_.overlay.workflow.project_search.selected_index];
    operations_.open_file_at_location(state_.root / result.relative_path, result.line, result.column);
    if (state_.sidebar.temporary) {
      operations_.restore_previous_sidebar();
    }
    state_.surface.focus = FocusTarget::Editor;
    operations_.seed_buffer_search_from_project_search();
  }
  return true;
}

bool SidebarMouseCoordinator::HandleSearchResultContextMenu(const SDL_Event& event,
                                                            const WorkspaceLayout& layout,
                                                            float local_y) {
  if (local_y < 0.0f) {
    return true;
  }
  const auto line_map = operations_.build_project_search_line_map();
  const auto list_layout =
      operations_.compute_project_search_sidebar_list_layout(layout.sidebar, line_map.size());
  const auto line_index =
      ScrollableListItemAtY(list_layout, static_cast<float>(event.button.y), line_map.size());
  if (!line_index.has_value() || line_map[*line_index] < 0) {
    return true;
  }
  // Select first, then open the menu on the selection — the same order the tree
  // and git sidebars use, so the menu and the highlight can never disagree.
  auto& search = state_.overlay.workflow.project_search;
  search.selected_index = static_cast<std::size_t>(line_map[*line_index]);
  operations_.open_tree_context_menu(TreeContextTargetKind::ResultRow,
                                     state_.root / search.results[search.selected_index].relative_path,
                                     PointerAnchor(event));
  return true;
}

bool SidebarMouseCoordinator::OpenSelectionContextMenu(const WorkspaceLayout& layout) {
  switch (operations_.active_sidebar_mode()) {
    case SidebarMode::Search: {
      const auto& search = state_.overlay.workflow.project_search;
      if (search.selected_index >= search.results.size()) {
        return false;
      }
      const std::vector<int> line_map = operations_.build_project_search_line_map();
      const auto line = std::find(line_map.begin(), line_map.end(), static_cast<int>(search.selected_index));
      const auto list = operations_.compute_project_search_sidebar_list_layout(layout.sidebar, line_map.size());
      operations_.open_tree_context_menu(
          TreeContextTargetKind::ResultRow, state_.root / search.results[search.selected_index].relative_path,
          RowAnchor(list, line == line_map.end() ? 0 : static_cast<int>(line - line_map.begin())));
      return true;
    }
    case SidebarMode::Git: {
      const auto& git = state_.sidebar.git;
      if (git.commit_workflow.open || git.selected_index >= git.entries.size()) {
        return false;
      }
      const auto& lines = operations_.build_git_sidebar_lines();
      const auto line = std::find_if(lines.begin(), lines.end(), [&](const GitSidebarLine& candidate) {
        return candidate.kind == GitSidebarLine::Kind::Entry &&
               candidate.entry_index == static_cast<int>(git.selected_index);
      });
      const auto list = operations_.compute_git_sidebar_list_layout(layout.sidebar, lines.size());
      operations_.open_tree_context_menu(
          TreeContextTargetKind::GitEntry, git.entries[git.selected_index].path,
          RowAnchor(list, line == lines.end() ? 0 : static_cast<int>(line - lines.begin())));
      return true;
    }
    case SidebarMode::Problems: {
      const auto& problems = state_.sidebar.problems;
      if (problems.selected_index >= problems.entries.size()) {
        return false;
      }
      const auto list = operations_.compute_problems_sidebar_list_layout(layout.sidebar, problems.entries.size());
      operations_.open_tree_context_menu(TreeContextTargetKind::ResultRow,
                                         problems.entries[problems.selected_index].diagnostic.path,
                                         RowAnchor(list, static_cast<int>(problems.selected_index)));
      return true;
    }
    case SidebarMode::Tests: {
      const auto& tests = state_.sidebar.tests;
      if (tests.selected_index >= tests.entries.size()) {
        return false;
      }
      const auto list = operations_.compute_tests_sidebar_list_layout(layout.sidebar, tests.entries.size());
      operations_.open_tree_context_menu(TreeContextTargetKind::ResultRow, tests.entries[tests.selected_index].file,
                                         RowAnchor(list, static_cast<int>(tests.selected_index)));
      return true;
    }
    case SidebarMode::Tree: {
      const auto& entries = state_.directory_tree.entries();
      const std::size_t selected = state_.directory_tree.selected_index();
      const auto list = operations_.compute_tree_sidebar_list_layout(layout.sidebar, entries.size());
      if (selected >= entries.size()) {
        operations_.open_tree_context_menu(TreeContextTargetKind::Background, {}, RowAnchor(list, 0));
        return true;
      }
      const auto& entry = entries[selected];
      const TreeContextTargetKind target = !entry.is_directory     ? TreeContextTargetKind::File
                                           : entry.path == state_.root ? TreeContextTargetKind::Root
                                                                       : TreeContextTargetKind::Directory;
      operations_.open_tree_context_menu(target, entry.path, RowAnchor(list, static_cast<int>(selected)));
      return true;
    }
    default:
      return false;
  }
}

bool SidebarMouseCoordinator::HandleGitButtonDown(const SDL_Event& event,
                                                  const WorkspaceLayout& layout,
                                                  float local_y) {
  const bool is_left = event.button.button == SDL_BUTTON_LEFT;
  const bool is_right = event.button.button == SDL_BUTTON_RIGHT;
  if (!is_left && !is_right) {
    return true;
  }
  // Header chrome (stage-all / discard-all / refresh / commit / outgoing base)
  // responds to left-click only.
  if (is_left) {
    const float px = static_cast<float>(event.button.x);
    const float py = static_cast<float>(event.button.y);
    if (operations_.can_stage_all_git_sidebar_entries() &&
        Contains(git_sidebar_header::StageAllButtonRect(layout.sidebar), px, py)) {
      // The click belongs to this button whether or not the git call succeeded;
      // reporting it as unhandled would hand the press on to the tab strip / panel
      // and skip the sidebar redraw that shows the outcome.
      operations_.stage_all_git_sidebar_entries();
      return true;
    }
    if (operations_.can_discard_all_git_sidebar_entries() &&
        Contains(git_sidebar_header::DiscardAllButtonRect(layout.sidebar), px, py)) {
      operations_.open_discard_all_git_sidebar_prompt();
      return true;
    }
    if (Contains(git_sidebar_header::RefreshButtonRect(layout.sidebar), px, py)) {
      return operations_.execute_action(ActionId::GitRefresh, {}, ActionSource::Shortcut);
    }
    // Branch row. Switching opens the picker (the argument-less form of the action);
    // Sync pulls then pushes. Both dispatch onto the background executor.
    if (Contains(git_sidebar_header::BranchButtonRect(layout.sidebar), px, py)) {
      return operations_.execute_action(ActionId::GitSwitchBranch, {}, ActionSource::Shortcut);
    }
    if (Contains(git_sidebar_header::SyncButtonRect(layout.sidebar), px, py)) {
      return operations_.execute_action(ActionId::GitSync, {}, ActionSource::Shortcut);
    }
    if (operations_.can_open_git_commit_button() &&
        Contains(git_sidebar_header::CommitButtonRect(layout.sidebar), px, py)) {
      return operations_.open_git_commit_workflow();
    }
    if (auto& workflow = state_.sidebar.git.commit_workflow;
        workflow.open && workflow.commit_button_rect.w > 0.0f &&
        Contains(workflow.commit_button_rect, event.button.x, event.button.y)) {
      return operations_.confirm_commit_workflow();
    }
    if (const auto button_rect = operations_.git_sidebar_outgoing_base_button_rect(layout.sidebar);
        button_rect.has_value() && Contains(*button_rect, event.button.x, event.button.y)) {
      operations_.open_anchored_menu(MenuId::GitOutgoingBase, *button_rect);
      return true;
    }
  }
  if (event.button.y < operations_.git_sidebar_list_top(layout.sidebar) || local_y < 0.0f) {
    return true;
  }

  const auto& lines = operations_.build_git_sidebar_lines();
  const auto list_layout = operations_.compute_git_sidebar_list_layout(layout.sidebar, lines.size());
  const auto line_index =
      ScrollableListItemAtY(list_layout, static_cast<float>(event.button.y), lines.size());
  if (!line_index.has_value()) {
    return true;
  }

  const auto& line = lines[*line_index];
  if (line.kind == GitSidebarLine::Kind::Directory) {
    if (!is_left) {
      return true;  // Right-click on a directory header is a no-op.
    }
    return operations_.toggle_git_sidebar_directory_collapsed != nullptr &&
           operations_.toggle_git_sidebar_directory_collapsed(line.tree_node_key);
  }
  if (line.kind != GitSidebarLine::Kind::Entry || line.entry_index < 0) {
    return true;
  }

  // Both buttons select the entry. Left-click opens its diff (DefaultView);
  // right-click opens the entry context menu (Stage/Unstage, Discard, …). The
  // menu acts on the selected entry, so selecting first keeps them in sync.
  state_.sidebar.git.selected_index = static_cast<std::size_t>(line.entry_index);
  if (is_right) {
    operations_.open_tree_context_menu(
        TreeContextTargetKind::GitEntry,
        state_.sidebar.git.entries[state_.sidebar.git.selected_index].path,
        PointerAnchor(event));
    return true;
  }
  // A click opens the entry's diff and keeps the keyboard on the list, as the
  // file tree's click does and VS Code's source control view does: the view's
  // own keys (s, u, d) then act on the entry just clicked. It used to hand the
  // keyboard to the opened editor, so an `s` meant to stage typed an "s" into
  // the file. Enter is the open that moves the keyboard to what it opened.
  const bool opened = operations_.dispatch_git_sidebar_action(GitSidebarActionId::DefaultView,
                                                              state_.sidebar.git.selected_index);
  state_.surface.focus = FocusTarget::Sidebar;
  return opened;
}

bool SidebarMouseCoordinator::HandleProblemsButtonDown(const SDL_Event& event,
                                                       const WorkspaceLayout& layout,
                                                       float local_y) {
  if (local_y < 0.0f) {
    return true;
  }

  const auto list_layout =
      operations_.compute_problems_sidebar_list_layout(layout.sidebar, state_.sidebar.problems.entries.size());
  const auto item_index = ScrollableListItemAtY(list_layout, static_cast<float>(event.button.y),
                                                state_.sidebar.problems.entries.size());
  if (!item_index.has_value()) {
    return true;
  }

  state_.sidebar.problems.selected_index = *item_index;
  operations_.reveal_selected_problems_sidebar_line();
  if (event.button.button == SDL_BUTTON_LEFT) {
    operations_.open_selected_problem_sidebar_item();
  } else if (event.button.button == SDL_BUTTON_RIGHT) {
    operations_.open_tree_context_menu(
        TreeContextTargetKind::ResultRow,
        state_.sidebar.problems.entries[state_.sidebar.problems.selected_index].diagnostic.path,
        PointerAnchor(event));
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
  const auto item_index = ScrollableListItemAtY(list_layout, static_cast<float>(event.button.y),
                                                state_.sidebar.tests.entries.size());
  if (!item_index.has_value()) {
    return true;
  }

  state_.sidebar.tests.selected_index = *item_index;
  operations_.reveal_selected_tests_sidebar_line();
  if (event.button.button == SDL_BUTTON_LEFT) {
    operations_.open_selected_test_sidebar_item();
  } else if (event.button.button == SDL_BUTTON_MIDDLE) {
    operations_.run_selected_test_sidebar_item();
  } else if (event.button.button == SDL_BUTTON_RIGHT) {
    operations_.open_tree_context_menu(
        TreeContextTargetKind::ResultRow,
        state_.sidebar.tests.entries[state_.sidebar.tests.selected_index].file,
        PointerAnchor(event));
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
  const auto item_index = ScrollableListItemAtY(list_layout, static_cast<float>(event.button.y),
                                                state_.sidebar.plugin.items.size());
  if (!item_index.has_value()) {
    return true;
  }

  state_.sidebar.plugin.selected_index = *item_index;
  operations_.reveal_selected_plugin_sidebar_line();
  if (event.button.button == SDL_BUTTON_LEFT) {
    // A click in the indentation + disclosure slot of a collapsible tree row
    // toggles it (re-snapshotting the provider) instead of confirming it. The
    // hit region must mirror the render geometry exactly (label_x in the
    // plugin branch of WorkspaceShellRenderSidebar.cpp).
    const auto& item = state_.sidebar.plugin.items[static_cast<std::size_t>(*item_index)];
    if (item.collapsible && operations_.toggle_plugin_sidebar_item) {
      const SDL_FRect row_rect =
          ScrollableListRowRect(list_layout, *item_index - list_layout.scroll_row);
      const float depth_offset =
          static_cast<float>(item.depth) * kWorkspaceSidebarTreeIndentWidth;
      const float label_x = row_rect.x + 6.0f + depth_offset +
                            kWorkspaceSidebarTreeChevronSlotWidth + 4.0f;
      if (static_cast<float>(event.button.x) < label_x) {
        operations_.toggle_plugin_sidebar_item();
        return true;
      }
    }
    operations_.open_selected_plugin_sidebar_item();
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
      operations_.open_tree_context_menu(TreeContextTargetKind::Background, {},
                                         PointerAnchor(event));
    }
    return true;
  }

  const auto& entries = state_.directory_tree.entries();
  const auto list_layout = operations_.compute_tree_sidebar_list_layout(layout.sidebar, entries.size());
  const auto entry_index =
      ScrollableListItemAtY(list_layout, static_cast<float>(event.button.y), entries.size());
  if (entry_index.has_value()) {
    state_.directory_tree.SetSelectedIndex(*entry_index);
    const SDL_FRect row_rect = ScrollableListItemRect(list_layout, *entry_index);
    if (Contains(row_rect, event.button.x, event.button.y) &&
        event.button.button == SDL_BUTTON_RIGHT) {
      const auto& entry = entries[*entry_index];
      const TreeContextTargetKind target =
          !entry.is_directory ? TreeContextTargetKind::File
          : entry.path == state_.root ? TreeContextTargetKind::Root
                                      : TreeContextTargetKind::Directory;
      operations_.open_tree_context_menu(target, entry.path, PointerAnchor(event));
      return true;
    }
    if (Contains(row_rect, event.button.x, event.button.y) && event.button.button == SDL_BUTTON_LEFT) {
      // The press selects and arms a drag; the release opens the row unless the
      // pointer moved away with it (FinishPathDrag).
      const auto& entry = entries[*entry_index];
      interaction_state_.path_drag = PathDragState{
          .armed = true,
          .path = entry.path,
          .is_directory = entry.is_directory,
          .label = entry.label,
          .press_x = static_cast<float>(event.button.x),
          .press_y = static_cast<float>(event.button.y),
          .pointer_x = static_cast<float>(event.button.x),
          .pointer_y = static_cast<float>(event.button.y),
      };
      operations_.reveal_selected_tree_sidebar_line();
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
