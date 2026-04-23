#include "workspace/WorkspaceKeyInputCoordinator.h"

#include <algorithm>
#include <limits>

namespace microide::workspace {

bool KeyInputCoordinator::HandleOverlayKeyDown(const SDL_KeyboardEvent& event,
                                               SDL_Keymod modifiers) {
  if (state_.overlay.mode == OverlayMode::CommitPicker) {
    switch (event.key) {
      case SDLK_ESCAPE:
        operations_.dismiss_overlay(false);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        operations_.activate_overlay_selection();
        return true;
      case SDLK_UP:
        operations_.move_compare_picker_selection(-1);
        return true;
      case SDLK_DOWN:
        operations_.move_compare_picker_selection(1);
        return true;
      case SDLK_HOME:
        if (!state_.overlay.workflow.compare_picker.matches.empty()) {
          state_.overlay.workflow.compare_picker.selected_index = 0;
          if (const auto layout = operations_.current_workspace_layout(); layout.has_value()) {
            operations_.reveal_overlay_selection(operations_.compute_overlay_rect(layout->editor_area));
          }
        }
        return true;
      case SDLK_END:
        if (!state_.overlay.workflow.compare_picker.matches.empty()) {
          state_.overlay.workflow.compare_picker.selected_index =
              state_.overlay.workflow.compare_picker.matches.size() - 1;
          if (const auto layout = operations_.current_workspace_layout(); layout.has_value()) {
            operations_.reveal_overlay_selection(operations_.compute_overlay_rect(layout->editor_area));
          }
        }
        return true;
      case SDLK_PAGEUP:
        operations_.move_compare_picker_selection(-8);
        return true;
      case SDLK_PAGEDOWN:
        operations_.move_compare_picker_selection(8);
        return true;
      default:
        return operations_.text_input_handle_single_line_key_down(event, modifiers);
    }
  }

  if (state_.overlay.mode == OverlayMode::BufferSearch) {
    switch (event.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        operations_.activate_overlay_selection();
        return true;
      case SDLK_UP:
        operations_.move_buffer_search_selection(-1);
        return true;
      case SDLK_DOWN:
        operations_.move_buffer_search_selection(1);
        return true;
      case SDLK_PAGEUP:
        operations_.move_buffer_search_selection(-8);
        return true;
      case SDLK_PAGEDOWN:
        operations_.move_buffer_search_selection(8);
        return true;
      default:
        return operations_.text_input_handle_single_line_key_down(event, modifiers);
    }
  }

  if (state_.overlay.mode == OverlayMode::BufferReplace) {
    switch (event.key) {
      case SDLK_ESCAPE:
        state_.overlay.visible = false;
        state_.surface.focus = FocusTarget::Editor;
        return true;
      case SDLK_TAB:
        state_.overlay.buffer_search_field =
            state_.overlay.buffer_search_field == BufferSearchField::Search
                ? BufferSearchField::Replace
                : BufferSearchField::Search;
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        if (modifiers & SDL_KMOD_CTRL) {
          operations_.replace_all_buffer_search_matches();
        } else {
          operations_.replace_current_buffer_search_match();
        }
        return true;
      case SDLK_UP:
        operations_.move_buffer_search_selection(-1);
        return true;
      case SDLK_DOWN:
        operations_.move_buffer_search_selection(1);
        return true;
      case SDLK_PAGEUP:
        operations_.move_buffer_search_selection(-8);
        return true;
      case SDLK_PAGEDOWN:
        operations_.move_buffer_search_selection(8);
        return true;
      default:
        return operations_.text_input_handle_single_line_key_down(event, modifiers);
    }
  }

  if (state_.overlay.mode == OverlayMode::ProjectSearch) {
    switch (event.key) {
      case SDLK_ESCAPE:
        state_.overlay.visible = false;
        state_.surface.focus = FocusTarget::Editor;
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        operations_.activate_overlay_selection();
        return true;
      case SDLK_UP:
        operations_.move_project_search_selection(-1);
        return true;
      case SDLK_DOWN:
        operations_.move_project_search_selection(1);
        return true;
      case SDLK_PAGEUP:
        operations_.move_project_search_selection(-8);
        return true;
      case SDLK_PAGEDOWN:
        operations_.move_project_search_selection(8);
        return true;
      default:
        return operations_.text_input_handle_single_line_key_down(event, modifiers);
    }
  }

  if (state_.overlay.mode == OverlayMode::Completion ||
      state_.overlay.mode == OverlayMode::CodeActions ||
      state_.overlay.mode == OverlayMode::TaskPicker) {
    auto set_selected_index = [&](std::size_t index, std::size_t item_count, std::size_t& target) {
      if (item_count == 0) {
        return;
      }
      target = std::min(index, item_count - 1);
      if (const auto layout = operations_.current_workspace_layout(); layout.has_value()) {
        operations_.reveal_overlay_selection(operations_.compute_overlay_rect(layout->editor_area));
      }
    };

    auto move_selection = [&](int delta) {
      if (state_.overlay.mode == OverlayMode::Completion &&
          !state_.overlay.workflow.completion.items.empty()) {
        const int current = static_cast<int>(state_.overlay.workflow.completion.selected_index);
        const int max_index =
            static_cast<int>(state_.overlay.workflow.completion.items.size()) - 1;
        state_.overlay.workflow.completion.selected_index =
            static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
      } else if (state_.overlay.mode == OverlayMode::CodeActions &&
                 !state_.overlay.workflow.code_actions.items.empty()) {
        const int current = static_cast<int>(state_.overlay.workflow.code_actions.selected_index);
        const int max_index =
            static_cast<int>(state_.overlay.workflow.code_actions.items.size()) - 1;
        state_.overlay.workflow.code_actions.selected_index =
            static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
      } else if (state_.overlay.mode == OverlayMode::TaskPicker &&
                 !state_.overlay.workflow.task_picker.entries.empty()) {
        const int current = static_cast<int>(state_.overlay.workflow.task_picker.selected_index);
        const int max_index =
            static_cast<int>(state_.overlay.workflow.task_picker.entries.size()) - 1;
        state_.overlay.workflow.task_picker.selected_index =
            static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
      }
      if (const auto layout = operations_.current_workspace_layout(); layout.has_value()) {
        operations_.reveal_overlay_selection(operations_.compute_overlay_rect(layout->editor_area));
      }
    };

    switch (event.key) {
      case SDLK_ESCAPE:
        operations_.dismiss_overlay(false);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        operations_.activate_overlay_selection();
        return true;
      case SDLK_UP:
        move_selection(-1);
        return true;
      case SDLK_DOWN:
        move_selection(1);
        return true;
      case SDLK_PAGEUP:
        move_selection(-8);
        return true;
      case SDLK_PAGEDOWN:
        move_selection(8);
        return true;
      case SDLK_HOME:
        if (state_.overlay.mode == OverlayMode::Completion) {
          set_selected_index(0, state_.overlay.workflow.completion.items.size(),
                             state_.overlay.workflow.completion.selected_index);
        } else if (state_.overlay.mode == OverlayMode::CodeActions) {
          set_selected_index(0, state_.overlay.workflow.code_actions.items.size(),
                             state_.overlay.workflow.code_actions.selected_index);
        } else {
          set_selected_index(0, state_.overlay.workflow.task_picker.entries.size(),
                             state_.overlay.workflow.task_picker.selected_index);
        }
        return true;
      case SDLK_END:
        if (state_.overlay.mode == OverlayMode::Completion &&
            !state_.overlay.workflow.completion.items.empty()) {
          set_selected_index(state_.overlay.workflow.completion.items.size() - 1,
                             state_.overlay.workflow.completion.items.size(),
                             state_.overlay.workflow.completion.selected_index);
        } else if (state_.overlay.mode == OverlayMode::CodeActions &&
                   !state_.overlay.workflow.code_actions.items.empty()) {
          set_selected_index(state_.overlay.workflow.code_actions.items.size() - 1,
                             state_.overlay.workflow.code_actions.items.size(),
                             state_.overlay.workflow.code_actions.selected_index);
        } else if (state_.overlay.mode == OverlayMode::TaskPicker &&
                   !state_.overlay.workflow.task_picker.entries.empty()) {
          set_selected_index(state_.overlay.workflow.task_picker.entries.size() - 1,
                             state_.overlay.workflow.task_picker.entries.size(),
                             state_.overlay.workflow.task_picker.selected_index);
        }
        return true;
      default:
        return false;
    }
  }

  switch (event.key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      operations_.activate_overlay_selection();
      return true;
    case SDLK_UP:
      operations_.move_file_finder_selection(-1);
      return true;
    case SDLK_DOWN:
      operations_.move_file_finder_selection(1);
      return true;
    case SDLK_PAGEUP:
      operations_.move_file_finder_selection(-8);
      return true;
    case SDLK_PAGEDOWN:
      operations_.move_file_finder_selection(8);
      return true;
    default:
      return operations_.text_input_handle_single_line_key_down(event, modifiers);
  }
}

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
        }
        return true;
      case SDLK_UP:
        operations_.move_project_search_selection(-1);
        return true;
      case SDLK_DOWN:
        operations_.move_project_search_selection(1);
        return true;
      case SDLK_HOME:
        if (!state_.overlay.workflow.project_search.results.empty()) {
          state_.overlay.workflow.project_search.selected_index = 0;
        }
        return true;
      case SDLK_END:
        if (!state_.overlay.workflow.project_search.results.empty()) {
          state_.overlay.workflow.project_search.selected_index =
              state_.overlay.workflow.project_search.results.size() - 1;
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

  if (sidebar_mode == SidebarMode::Chat) {
    switch (event.key) {
      case SDLK_ESCAPE:
        if (state_.sidebar.temporary) {
          operations_.close_sidebar();
        } else {
          state_.surface.focus = FocusTarget::Editor;
        }
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return operations_.start_chat_request({});
      case SDLK_PAGEUP:
        state_.sidebar.scroll_row = std::max(0, state_.sidebar.scroll_row - 8);
        return true;
      case SDLK_PAGEDOWN:
        state_.sidebar.scroll_row += 8;
        return true;
      case SDLK_HOME:
        state_.sidebar.scroll_row = 0;
        return true;
      case SDLK_END:
        state_.sidebar.scroll_row = std::numeric_limits<int>::max();
        return true;
      default:
        return operations_.text_input_handle_single_line_key_down(event, modifiers);
    }
  }

  if (sidebar_mode == SidebarMode::Git) {
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
        return operations_.open_git_sidebar_entry(state_.sidebar.git.selected_index);
      case SDLK_R:
        return operations_.execute_action(ActionId::GitRefresh, {}, ActionSource::Shortcut);
      default: {
        const char input_character = operations_.keycode_to_ascii(event.key, modifiers);
        if (input_character == 's') {
          return operations_.stage_git_sidebar_entry(state_.sidebar.git.selected_index);
        }
        if (input_character == 'u') {
          return operations_.unstage_git_sidebar_entry(state_.sidebar.git.selected_index);
        }
        if (input_character == 'x') {
          return operations_.discard_git_sidebar_entry(state_.sidebar.git.selected_index);
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

  if (sidebar_mode == SidebarMode::Plugin) {
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
      case SDLK_RIGHT:
        return operations_.open_selected_plugin_sidebar_item();
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
