#include "workspace/WorkspaceKeyInputCoordinator.h"

#include <algorithm>
#include <array>
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
        // Route through the canonical dismissal so fold-reveal cleanup, cursor-kind
        // invalidation, and focus restoration all happen (a bare visible=false here
        // left temporarily-expanded folds open and the cursor kind stale).
        operations_.dismiss_overlay(true);
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
        operations_.dismiss_overlay(true);
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
      state_.overlay.mode == OverlayMode::CodeActions) {
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


}  // namespace microide::workspace
