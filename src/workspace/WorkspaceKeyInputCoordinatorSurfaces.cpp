#include "workspace/WorkspaceKeyInputCoordinator.h"

#include <algorithm>
#include <array>
#include <limits>

#include "workspace/ListSelection.h"

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
        operations_.move_compare_picker_selection(-kListPageStep);
        return true;
      case SDLK_PAGEDOWN:
        operations_.move_compare_picker_selection(kListPageStep);
        return true;
      default:
        return operations_.text_input_handle_single_line_key_down(event, modifiers);
    }
  }

  if (state_.overlay.mode == OverlayMode::BufferSearch) {
    switch (event.key) {
      case SDLK_ESCAPE:
        // Non-modal find: Esc closes the floating widget and returns focus to the
        // editor. Route through the canonical dismissal so fold-reveal cleanup and
        // cursor-kind invalidation happen.
        operations_.dismiss_overlay(true);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        // Enter cycles to the next match (Shift+Enter to the previous) and keeps
        // the widget open, VSCode-style, instead of jumping + dismissing.
        operations_.move_buffer_search_selection((modifiers & SDL_KMOD_SHIFT) ? -1 : 1);
        return true;
      case SDLK_UP:
        operations_.move_buffer_search_selection(-1);
        return true;
      case SDLK_DOWN:
        operations_.move_buffer_search_selection(1);
        return true;
      case SDLK_PAGEUP:
        operations_.move_buffer_search_selection(-kListPageStep);
        return true;
      case SDLK_PAGEDOWN:
        operations_.move_buffer_search_selection(kListPageStep);
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
        operations_.move_buffer_search_selection(-kListPageStep);
        return true;
      case SDLK_PAGEDOWN:
        operations_.move_buffer_search_selection(kListPageStep);
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
        operations_.move_project_search_selection(-kListPageStep);
        return true;
      case SDLK_PAGEDOWN:
        operations_.move_project_search_selection(kListPageStep);
        return true;
      default:
        return operations_.text_input_handle_single_line_key_down(event, modifiers);
    }
  }

  if (state_.overlay.mode == OverlayMode::CommandPalette) {
    auto& palette = state_.overlay.workflow.command_palette;
    const std::size_t item_count = palette.matches.size();
    const auto apply_selection = [&](std::size_t index) {
      if (item_count == 0) {
        return;
      }
      palette.selected_index = std::min(index, item_count - 1);
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
        apply_selection(ClampListIndexMove(palette.selected_index, item_count, -1));
        return true;
      case SDLK_DOWN:
        apply_selection(ClampListIndexMove(palette.selected_index, item_count, 1));
        return true;
      case SDLK_PAGEUP:
        apply_selection(ClampListIndexMove(palette.selected_index, item_count, -kListPageStep));
        return true;
      case SDLK_PAGEDOWN:
        apply_selection(ClampListIndexMove(palette.selected_index, item_count, kListPageStep));
        return true;
      case SDLK_HOME:
        apply_selection(0);
        return true;
      case SDLK_END:
        if (item_count > 0) {
          apply_selection(item_count - 1);
        }
        return true;
      default:
        return operations_.text_input_handle_single_line_key_down(event, modifiers);
    }
  }

  if (state_.overlay.mode == OverlayMode::LaunchConfigPicker) {
    auto& picker = state_.overlay.workflow.launch_config_picker;
    const std::size_t item_count = picker.matches.size();
    const auto apply_selection = [&](std::size_t index) {
      if (item_count == 0) {
        return;
      }
      picker.selected_index = std::min(index, item_count - 1);
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
        apply_selection(ClampListIndexMove(picker.selected_index, item_count, -1));
        return true;
      case SDLK_DOWN:
        apply_selection(ClampListIndexMove(picker.selected_index, item_count, 1));
        return true;
      case SDLK_PAGEUP:
        apply_selection(ClampListIndexMove(picker.selected_index, item_count, -kListPageStep));
        return true;
      case SDLK_PAGEDOWN:
        apply_selection(ClampListIndexMove(picker.selected_index, item_count, kListPageStep));
        return true;
      case SDLK_HOME:
        apply_selection(0);
        return true;
      case SDLK_END:
        if (item_count > 0) {
          apply_selection(item_count - 1);
        }
        return true;
      default:
        return operations_.text_input_handle_single_line_key_down(event, modifiers);
    }
  }

  if (state_.overlay.mode == OverlayMode::Completion ||
      state_.overlay.mode == OverlayMode::CodeActions) {
    // Completion and code-actions are the same list widget over different item vectors;
    // bind the active list once and drive it through the shared clamped-move helper.
    const bool is_completion = state_.overlay.mode == OverlayMode::Completion;
    const std::size_t item_count = is_completion
                                       ? state_.overlay.workflow.completion.items.size()
                                       : state_.overlay.workflow.code_actions.items.size();
    std::size_t& selected_index = is_completion
                                      ? state_.overlay.workflow.completion.selected_index
                                      : state_.overlay.workflow.code_actions.selected_index;
    const auto apply_selection = [&](std::size_t index) {
      if (item_count == 0) {
        return;
      }
      selected_index = std::min(index, item_count - 1);
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
        apply_selection(ClampListIndexMove(selected_index, item_count, -1));
        return true;
      case SDLK_DOWN:
        apply_selection(ClampListIndexMove(selected_index, item_count, 1));
        return true;
      case SDLK_PAGEUP:
        apply_selection(ClampListIndexMove(selected_index, item_count, -kListPageStep));
        return true;
      case SDLK_PAGEDOWN:
        apply_selection(ClampListIndexMove(selected_index, item_count, kListPageStep));
        return true;
      case SDLK_HOME:
        apply_selection(0);
        return true;
      case SDLK_END:
        if (item_count > 0) {
          apply_selection(item_count - 1);
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
      operations_.move_file_finder_selection(-kListPageStep);
      return true;
    case SDLK_PAGEDOWN:
      operations_.move_file_finder_selection(kListPageStep);
      return true;
    default:
      return operations_.text_input_handle_single_line_key_down(event, modifiers);
  }
}


}  // namespace microide::workspace
