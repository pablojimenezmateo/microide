#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"

#include <cstddef>
#include <functional>
#include <vector>

#include "project/CommitWorkflowTypes.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/ListSelection.h"

namespace microide::workspace {
bool KeyInputCoordinator::HandleSidebarKeyDown(const SDL_KeyboardEvent& event,
                                               SDL_Keymod modifiers) {
  const SidebarMode sidebar_mode = operations_.active_sidebar_mode();

  const bool field_active =
      (sidebar_mode == SidebarMode::Search && state_.overlay.workflow.project_search.editing) ||
      (sidebar_mode == SidebarMode::Git && state_.sidebar.git.commit_workflow.open);
  const bool context_menu_key =
      event.key == SDLK_APPLICATION || event.key == SDLK_MENU ||
      (event.key == SDLK_F10 && NormalizedKeyModifiers(modifiers) == SDL_KMOD_SHIFT);
  if (context_menu_key && !field_active && operations_.open_sidebar_selection_context_menu) {
    return operations_.open_sidebar_selection_context_menu();
  }

  if (event.key == SDLK_ESCAPE) {
    const bool sub_editor_active =
        (sidebar_mode == SidebarMode::Search &&
         state_.overlay.workflow.project_search.editing) ||
        (sidebar_mode == SidebarMode::Git && state_.sidebar.git.commit_workflow.open);
    if (!sub_editor_active) {
      if (!state_.sidebar.temporary) {
        return false;
      }
      operations_.close_sidebar();
      return true;
    }
  }

  if (sidebar_mode == SidebarMode::Search) {
    const char input_character = operations_.keycode_to_ascii(event.key, modifiers);
    auto& search_state = state_.overlay.workflow.project_search;
    const auto visible_fields = [&search_state]() {
      std::vector<ProjectSearchEditField> fields{ProjectSearchEditField::Query,
                                                 ProjectSearchEditField::Replace};
      if (search_state.scope_expanded) {
        fields.push_back(ProjectSearchEditField::Include);
        fields.push_back(ProjectSearchEditField::Exclude);
      }
      return fields;
    };

    if ((modifiers & SDL_KMOD_ALT) != 0) {
      const std::function<void()>* toggle =
          event.key == SDLK_R   ? &operations_.toggle_project_search_pattern_mode
          : event.key == SDLK_C ? &operations_.cycle_project_search_case_mode
          : event.key == SDLK_H ? &operations_.toggle_project_search_hidden_files
                                : nullptr;
      if (toggle != nullptr && *toggle != nullptr) {
        (*toggle)();
        return true;
      }
    }

    if (search_state.editing) {
      switch (event.key) {
        case SDLK_ESCAPE:
          operations_.cancel_project_search_edit();
          return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
          if (search_state.edit_field == ProjectSearchEditField::Replace &&
              (modifiers & SDL_KMOD_CTRL) != 0 && (modifiers & SDL_KMOD_ALT) != 0) {
            operations_.replace_all_project_search_matches();
            return true;
          }
          operations_.commit_project_search_edit();
          return true;
        case SDLK_TAB: {
          const std::vector<ProjectSearchEditField> fields = visible_fields();
          std::size_t index = 0;
          for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i] == search_state.edit_field) {
              index = i;
              break;
            }
          }
          const std::size_t count = fields.size();
          const std::size_t next = (modifiers & SDL_KMOD_SHIFT) != 0
                                       ? (index + count - 1) % count
                                       : (index + 1) % count;
          operations_.commit_project_search_edit();
          operations_.begin_project_search_edit(fields[next]);
          return true;
        }
        default:
          return operations_.text_input_handle_single_line_key_down(event, modifiers);
      }
    }

    if (const auto delta = ListNavigationKeyDelta(
            event.key, state_.overlay.workflow.project_search.results.size());
        delta.has_value()) {
      operations_.move_project_search_selection(*delta);
      return true;
    }

    switch (event.key) {
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
            viewport->JumpCursorTo(result.line, result.column);
          }
          if (state_.sidebar.temporary) {
            operations_.restore_previous_sidebar();
          }
          state_.surface.focus = FocusTarget::Editor;
          operations_.seed_buffer_search_from_project_search();
        }
        return true;
      default:
        break;
    }
    if ((modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 && input_character > ' ' &&
        input_character < 127) {
      const auto& results = search_state.results;
      const auto preview = [&results](std::size_t i) {
        std::string_view text = results[i].preview;
        while (!text.empty() && text.front() == ' ') {
          text.remove_prefix(1);
        }
        return text;
      };
      if (const auto index = TypeNavigate(search_state.type_navigation, input_character, event.timestamp,
                                          search_state.selected_index, results.size(), preview);
          index.has_value()) {
        operations_.move_project_search_selection(static_cast<int>(*index) -
                                                  static_cast<int>(search_state.selected_index));
      }
      return true;
    }
    return false;
  }

  if (sidebar_mode == SidebarMode::Git) {
    if (state_.sidebar.git.commit_workflow.open) {
      auto& workflow = state_.sidebar.git.commit_workflow;
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
      return operations_.text_input_handle_single_line_key_down(event, modifiers);
    }
    const std::size_t selected_index = state_.sidebar.git.selected_index;
    if (const auto delta =
            ListNavigationKeyDelta(event.key, state_.sidebar.git.entries.size());
        delta.has_value()) {
      operations_.move_git_sidebar_selection(*delta);
      return true;
    }
    switch (event.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return operations_.dispatch_git_sidebar_action(GitSidebarActionId::DefaultView,
                                                       selected_index);
      default:
        break;
    }
    if ((modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0) {
      const char typed = operations_.keycode_to_ascii(event.key, modifiers);
      if (typed > ' ' && typed < 127) {
        if (operations_.type_navigate_git_sidebar) {
          (void)operations_.type_navigate_git_sidebar(typed, event.timestamp);
        }
        return true;
      }
    }
    return false;
  }

  if (sidebar_mode == SidebarMode::Problems) {
    if (const auto delta =
            ListNavigationKeyDelta(event.key, state_.sidebar.problems.entries.size());
        delta.has_value()) {
      operations_.move_problems_sidebar_selection(*delta);
      return true;
    }
    switch (event.key) {
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
    if (const auto delta =
            ListNavigationKeyDelta(event.key, state_.sidebar.tests.entries.size());
        delta.has_value()) {
      operations_.move_tests_sidebar_selection(*delta);
      return true;
    }
    switch (event.key) {
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

  if (UsesPluginItemSurface(sidebar_mode)) {
    const auto* selected_plugin_item =
        state_.sidebar.plugin.selected_index < state_.sidebar.plugin.items.size()
            ? &state_.sidebar.plugin.items[state_.sidebar.plugin.selected_index]
            : nullptr;
    const bool selection_is_collapsible =
        selected_plugin_item != nullptr && selected_plugin_item->collapsible;
    if (const auto delta =
            ListNavigationKeyDelta(event.key, state_.sidebar.plugin.items.size());
        delta.has_value()) {
      operations_.move_plugin_sidebar_selection(*delta);
      return true;
    }
    switch (event.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return operations_.open_selected_plugin_sidebar_item();
      case SDLK_RIGHT:
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
      case SDLK_DELETE:
        return operations_.end_selected_plugin_sidebar_item && operations_.end_selected_plugin_sidebar_item();
      default:
        return false;
    }
  }

  if (const auto delta =
          ListNavigationKeyDelta(event.key, state_.directory_tree.entries().size());
      delta.has_value()) {
    state_.directory_tree.MoveSelection(*delta);
    operations_.reveal_selected_tree_sidebar_line();
    return true;
  }
  if (NormalizedKeyModifiers(modifiers) == SDL_KMOD_CTRL &&
      (event.key == SDLK_X || event.key == SDLK_C || event.key == SDLK_V)) {
    (void)operations_.execute_action(event.key == SDLK_X   ? ActionId::TreeCut
                                     : event.key == SDLK_C ? ActionId::TreeCopy
                                                           : ActionId::TreePaste,
                                     {}, ActionSource::Shortcut);
    return true;
  }
  switch (event.key) {
    case SDLK_F2:
      (void)operations_.execute_action(ActionId::RenamePath, {}, ActionSource::Shortcut);
      return true;
    case SDLK_DELETE:
      (void)operations_.execute_action(ActionId::DeletePath, {}, ActionSource::Shortcut);
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
        state_.surface.focus = FocusTarget::Sidebar;
      }
      return true;
    }
    default:
      break;
  }
  if ((modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0) {
    const char typed = operations_.keycode_to_ascii(event.key, modifiers);
    if (typed > ' ' && typed < 127) {
      const auto& entries = state_.directory_tree.entries();
      if (const auto index = TypeNavigate(
              state_.sidebar.tree_type_navigation, typed, event.timestamp,
              state_.directory_tree.selected_index(), entries.size(),
              [&entries](std::size_t i) { return std::string_view(entries[i].label); });
          index.has_value()) {
        state_.directory_tree.SetSelectedIndex(*index);
        operations_.reveal_selected_tree_sidebar_line();
      }
      return true;
    }
  }
  return false;
}


}
