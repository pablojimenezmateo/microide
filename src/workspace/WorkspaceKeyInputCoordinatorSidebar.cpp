#include "workspace/WorkspaceKeyInputCoordinator.h"

#include "project/CommitWorkflowTypes.h"
#include "workspace/GitSidebarCommandCenter.h"

namespace microide::workspace {
bool KeyInputCoordinator::HandleSidebarKeyDown(const SDL_KeyboardEvent& event,
                                               SDL_Keymod modifiers) {
  const SidebarMode sidebar_mode = operations_.active_sidebar_mode();
  if (sidebar_mode == SidebarMode::Search) {
    const char input_character = operations_.keycode_to_ascii(event.key, modifiers);
    if (state_.overlay.workflow.project_search.editing) {
      switch (event.key) {
        case SDLK_ESCAPE:
          operations_.cancel_project_search_edit();
          return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
          operations_.commit_project_search_edit();
          return true;
        default:
          return operations_.text_input_handle_single_line_key_down(event, modifiers);
      }
    }

    switch (event.key) {
      case SDLK_ESCAPE:
        if (state_.sidebar.temporary) {
          operations_.close_sidebar();
          return true;
        }
        return false;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
      case SDLK_RIGHT:
        if (!state_.overlay.workflow.project_search.results.empty() &&
            state_.overlay.workflow.project_search.selected_index <
                state_.overlay.workflow.project_search.results.size()) {
          const auto& result =
              state_.overlay.workflow.project_search.results[state_.overlay.workflow.project_search.selected_index];
          operations_.open_file(state_.root / result.relative_path);
          if (editor::TextViewport* viewport = operations_.active_editor_viewport();
              viewport != nullptr) {
            viewport->MoveCursorTo(result.line, result.column);
          }
          if (state_.sidebar.temporary) {
            operations_.restore_previous_sidebar();
          }
          state_.surface.focus = FocusTarget::Editor;
          operations_.seed_buffer_search_from_project_search();
        }
        return true;
      case SDLK_UP:
        operations_.move_project_search_selection(-1);
        return true;
      case SDLK_DOWN:
        operations_.move_project_search_selection(1);
        return true;
      case SDLK_HOME:
        // Route through move-selection (clamped) so the reveal-into-view logic runs.
        if (!state_.overlay.workflow.project_search.results.empty()) {
          operations_.move_project_search_selection(
              -static_cast<int>(state_.overlay.workflow.project_search.results.size()));
        }
        return true;
      case SDLK_END:
        if (!state_.overlay.workflow.project_search.results.empty()) {
          operations_.move_project_search_selection(
              static_cast<int>(state_.overlay.workflow.project_search.results.size()));
        }
        return true;
      case SDLK_PAGEUP:
        operations_.move_project_search_selection(-8);
        return true;
      case SDLK_PAGEDOWN:
        operations_.move_project_search_selection(8);
        return true;
      case SDLK_R:
        if (input_character == 'R') {
          operations_.replace_all_project_search_matches();
        } else {
          operations_.refresh_project_search();
        }
        return true;
      case SDLK_C:
        // Toggle "count all matches" (exact total vs. fast early-stop). The option
        // lives on the search state and a rerun applies it, so this needs no
        // dedicated shell entry point.
        if (input_character == 'c') {
          auto& options = state_.overlay.workflow.project_search.options;
          options.count_all_matches = !options.count_all_matches;
          operations_.refresh_project_search();
          return true;
        }
        return false;
      case SDLK_EQUALS:
        operations_.begin_project_search_edit(ProjectSearchEditField::Replace);
        return true;
      case SDLK_SLASH:
        operations_.begin_project_search_edit(ProjectSearchEditField::Query);
        return true;
      default:
        if (event.key == SDLK_J && input_character == 'j') {
          operations_.move_project_search_selection(1);
          return true;
        }
        if (event.key == SDLK_K && input_character == 'k') {
          operations_.move_project_search_selection(-1);
          return true;
        }
        return false;
    }
  }

  if (sidebar_mode == SidebarMode::Git) {
    if (state_.sidebar.git.commit_workflow.open) {
      auto& workflow = state_.sidebar.git.commit_workflow;
      // Ctrl+Enter commits from either field; plain Enter is a newline in the body and a
      // no-op in the single-line subject.
      if ((event.key == SDLK_RETURN || event.key == SDLK_KP_ENTER) &&
          (modifiers & SDL_KMOD_CTRL) != 0 && operations_.request_commit_workflow_commit != nullptr) {
        return operations_.request_commit_workflow_commit();
      }
      switch (event.key) {
        case SDLK_ESCAPE:
          if (operations_.close_commit_workflow != nullptr) {
            operations_.close_commit_workflow();
          }
          return true;
        case SDLK_TAB:
          // Tab moves between subject and body (Shift+Tab always returns to the subject),
          // keeping the panel keyboard-navigable like every other multi-field surface.
          if (operations_.set_commit_workflow_focus_field != nullptr) {
            const CommitWorkflowFocusField next =
                (modifiers & SDL_KMOD_SHIFT) != 0
                    ? CommitWorkflowFocusField::Subject
                    : (workflow.focus_field == CommitWorkflowFocusField::Subject
                           ? CommitWorkflowFocusField::Body
                           : CommitWorkflowFocusField::Subject);
            operations_.set_commit_workflow_focus_field(next);
          }
          return true;
        default:
          break;
      }
      if (workflow.focus_field == CommitWorkflowFocusField::Body) {
        return HandleCommitBodyKeyDown(event, modifiers);
      }
      // The subject is a single-line field: hand navigation/editing keys to the shared
      // single-line handler. Plain character keys return false here and are inserted by the
      // separate SDL_TextInput event; returning early keeps git-action keys (s/c/d/...) from
      // firing while the workflow owns the keyboard.
      return operations_.text_input_handle_single_line_key_down(event, modifiers);
    }
    const std::size_t selected_index = state_.sidebar.git.selected_index;
    switch (event.key) {
      case SDLK_UP:
        operations_.move_git_sidebar_selection(-1);
        return true;
      case SDLK_DOWN:
        operations_.move_git_sidebar_selection(1);
        return true;
      case SDLK_HOME:
        if (!state_.sidebar.git.entries.empty()) {
          state_.sidebar.git.selected_index = 0;
          operations_.reveal_selected_git_sidebar_line();
        }
        return true;
      case SDLK_END:
        if (!state_.sidebar.git.entries.empty()) {
          state_.sidebar.git.selected_index = state_.sidebar.git.entries.size() - 1;
          operations_.reveal_selected_git_sidebar_line();
        }
        return true;
      case SDLK_PAGEUP:
        operations_.move_git_sidebar_selection(-8);
        return true;
      case SDLK_PAGEDOWN:
        operations_.move_git_sidebar_selection(8);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return operations_.dispatch_git_sidebar_action(GitSidebarActionId::DefaultView,
                                                       selected_index);
      case SDLK_R:
        return operations_.dispatch_git_sidebar_action(GitSidebarActionId::Refresh,
                                                       selected_index);
      default: {
        const char input_character = operations_.keycode_to_ascii(event.key, modifiers);
        if (input_character == 'd') {
          return operations_.dispatch_git_sidebar_action(GitSidebarActionId::Diff, selected_index);
        }
        if (input_character == 's') {
          return operations_.dispatch_git_sidebar_action(GitSidebarActionId::Stage, selected_index);
        }
        if (input_character == 'u') {
          return operations_.dispatch_git_sidebar_action(GitSidebarActionId::Unstage,
                                                         selected_index);
        }
        if (input_character == 'x') {
          return operations_.dispatch_git_sidebar_action(GitSidebarActionId::Discard,
                                                         selected_index);
        }
        if (input_character == 'm') {
          return operations_.dispatch_git_sidebar_action(GitSidebarActionId::Merge, selected_index);
        }
        if (input_character == 'c') {
          return operations_.dispatch_git_sidebar_action(GitSidebarActionId::Commit,
                                                         selected_index);
        }
        if (input_character == 'o') {
          return operations_.dispatch_git_sidebar_action(GitSidebarActionId::OpenFile,
                                                         selected_index);
        }
        return false;
      }
    }
  }

  if (sidebar_mode == SidebarMode::Problems) {
    switch (event.key) {
      case SDLK_ESCAPE:
        if (state_.sidebar.temporary) {
          operations_.close_sidebar();
          return true;
        }
        return false;
      case SDLK_UP:
        operations_.move_problems_sidebar_selection(-1);
        return true;
      case SDLK_DOWN:
        operations_.move_problems_sidebar_selection(1);
        return true;
      case SDLK_HOME:
        if (!state_.sidebar.problems.entries.empty()) {
          state_.sidebar.problems.selected_index = 0;
          operations_.reveal_selected_problems_sidebar_line();
        }
        return true;
      case SDLK_END:
        if (!state_.sidebar.problems.entries.empty()) {
          state_.sidebar.problems.selected_index = state_.sidebar.problems.entries.size() - 1;
          operations_.reveal_selected_problems_sidebar_line();
        }
        return true;
      case SDLK_PAGEUP:
        operations_.move_problems_sidebar_selection(-8);
        return true;
      case SDLK_PAGEDOWN:
        operations_.move_problems_sidebar_selection(8);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
      case SDLK_RIGHT:
        return operations_.open_selected_problem_sidebar_item();
      case SDLK_R:
        return operations_.refresh_problems_sidebar();
      default:
        return false;
    }
  }

  if (sidebar_mode == SidebarMode::Tests) {
    switch (event.key) {
      case SDLK_ESCAPE:
        if (state_.sidebar.temporary) {
          operations_.close_sidebar();
          return true;
        }
        return false;
      case SDLK_UP:
        operations_.move_tests_sidebar_selection(-1);
        return true;
      case SDLK_DOWN:
        operations_.move_tests_sidebar_selection(1);
        return true;
      case SDLK_HOME:
        if (!state_.sidebar.tests.entries.empty()) {
          state_.sidebar.tests.selected_index = 0;
          operations_.reveal_selected_tests_sidebar_line();
        }
        return true;
      case SDLK_END:
        if (!state_.sidebar.tests.entries.empty()) {
          state_.sidebar.tests.selected_index = state_.sidebar.tests.entries.size() - 1;
          operations_.reveal_selected_tests_sidebar_line();
        }
        return true;
      case SDLK_PAGEUP:
        operations_.move_tests_sidebar_selection(-8);
        return true;
      case SDLK_PAGEDOWN:
        operations_.move_tests_sidebar_selection(8);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
      case SDLK_RIGHT:
        return operations_.open_selected_test_sidebar_item();
      case SDLK_R: {
        const char input_character = operations_.keycode_to_ascii(event.key, modifiers);
        if (input_character == 'r') {
          return operations_.run_selected_test_sidebar_item();
        }
        return operations_.refresh_tests_sidebar();
      }
      default:
        return false;
    }
  }

  if (sidebar_mode == SidebarMode::Plugin || sidebar_mode == SidebarMode::Outline) {
    const auto* selected_plugin_item =
        state_.sidebar.plugin.selected_index < state_.sidebar.plugin.items.size()
            ? &state_.sidebar.plugin.items[state_.sidebar.plugin.selected_index]
            : nullptr;
    const bool selection_is_collapsible =
        selected_plugin_item != nullptr && selected_plugin_item->collapsible;
    switch (event.key) {
      case SDLK_ESCAPE:
        if (state_.sidebar.temporary) {
          operations_.close_sidebar();
          return true;
        }
        return false;
      case SDLK_UP:
        operations_.move_plugin_sidebar_selection(-1);
        return true;
      case SDLK_DOWN:
        operations_.move_plugin_sidebar_selection(1);
        return true;
      case SDLK_HOME:
        if (!state_.sidebar.plugin.items.empty()) {
          state_.sidebar.plugin.selected_index = 0;
          operations_.reveal_selected_plugin_sidebar_line();
        }
        return true;
      case SDLK_END:
        if (!state_.sidebar.plugin.items.empty()) {
          state_.sidebar.plugin.selected_index = state_.sidebar.plugin.items.size() - 1;
          operations_.reveal_selected_plugin_sidebar_line();
        }
        return true;
      case SDLK_PAGEUP:
        operations_.move_plugin_sidebar_selection(-8);
        return true;
      case SDLK_PAGEDOWN:
        operations_.move_plugin_sidebar_selection(8);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return operations_.open_selected_plugin_sidebar_item();
      case SDLK_RIGHT:
        // Expand a collapsed tree row; otherwise preserve the flat-list behavior
        // where Right opens (confirms) the selection.
        if (selection_is_collapsible && selected_plugin_item->collapsed) {
          return operations_.toggle_plugin_sidebar_item();
        }
        return operations_.open_selected_plugin_sidebar_item();
      case SDLK_LEFT:
        if (selection_is_collapsible && !selected_plugin_item->collapsed) {
          return operations_.toggle_plugin_sidebar_item();
        }
        return false;
      case SDLK_SPACE:
        if (selection_is_collapsible) {
          return operations_.toggle_plugin_sidebar_item();
        }
        return false;
      case SDLK_R:
        return operations_.refresh_plugin_sidebar();
      default:
        return false;
    }
  }

  switch (event.key) {
    case SDLK_UP:
      state_.directory_tree.MoveSelection(-1);
      operations_.reveal_selected_tree_sidebar_line();
      return true;
    case SDLK_DOWN:
      state_.directory_tree.MoveSelection(1);
      operations_.reveal_selected_tree_sidebar_line();
      return true;
    case SDLK_LEFT:
      state_.directory_tree.CollapseSelection();
      operations_.reveal_selected_tree_sidebar_line();
      return true;
    case SDLK_RIGHT:
      state_.directory_tree.ExpandSelection();
      operations_.reveal_selected_tree_sidebar_line();
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: {
      const auto opened = state_.directory_tree.ActivateSelection();
      operations_.reveal_selected_tree_sidebar_line();
      if (opened.has_value()) {
        operations_.open_file(*opened);
        // Keep keyboard focus on the tree after opening so arrow/Enter
        // navigation continues without a manual refocus.
        state_.surface.focus = FocusTarget::Sidebar;
      }
      return true;
    }
    case SDLK_R:
      operations_.refresh_project_files();
      return true;
    case SDLK_D:
      operations_.open_compare_picker();
      return true;
    default:
      return false;
  }
}


}  // namespace microide::workspace
