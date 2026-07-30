#include "workspace/WorkspaceKeyInputCoordinator.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>

#include "workspace/ListSelection.h"

namespace microide::workspace {

namespace {

// The four keys every overlay list answers identically, mapped to the row delta they
// move by. Anything else is not list navigation and belongs to the mode (or, for the
// query-backed overlays, to the text field underneath).
//
// Home/End are deliberately absent: in the overlays that carry a query field they move
// the *text caret*, exactly as they do in VS Code's quick input, so Ctrl-less Home in
// the command palette jumps to the start of the command being typed instead of to the
// first result. Only the two field-less popups (completion, code actions) map them to
// first/last row.
std::optional<int> ListNavigationDelta(SDL_Keycode key) {
  switch (key) {
    case SDLK_UP:
      return -1;
    case SDLK_DOWN:
      return 1;
    case SDLK_PAGEUP:
      return -kListPageStep;
    case SDLK_PAGEDOWN:
      return kListPageStep;
    default:
      return std::nullopt;
  }
}

}  // namespace

void KeyInputCoordinator::MoveOverlayListSelection(std::size_t& selected_index,
                                                   std::size_t item_count,
                                                   int delta) {
  if (item_count == 0) {
    return;
  }
  selected_index = ClampListIndexMove(selected_index, item_count, delta);
  if (const auto layout = operations_.current_workspace_layout(); layout.has_value()) {
    operations_.reveal_overlay_selection(operations_.compute_overlay_rect(layout->editor_area));
  }
}

// Match navigation and the option chords behave identically in the find widget
// and the find-and-replace widget. They used to be two copies of the same switch
// arms, so a fix to one silently left the other behind. Anything not handled here
// falls through to the focused single-line field, which is also the correct
// behavior for a plain `c`/`w`/`r`.
bool KeyInputCoordinator::HandleSharedBufferSearchKey(const SDL_KeyboardEvent& event,
                                                      SDL_Keymod modifiers) {
  if (const auto delta = ListNavigationDelta(event.key); delta.has_value()) {
    operations_.move_buffer_search_selection(*delta);
    return true;
  }
  // Alt+C / Alt+W / Alt+R toggle match case, whole word and regex — VSCode's
  // find-widget chords, one per button, in button order. A plain letter still
  // types into the focused field via the default text-input path.
  if ((modifiers & SDL_KMOD_ALT) != 0) {
    const std::optional<BufferFindToggle> toggle =
        event.key == SDLK_C   ? std::optional(BufferFindToggle::MatchCase)
        : event.key == SDLK_W ? std::optional(BufferFindToggle::WholeWord)
        : event.key == SDLK_R ? std::optional(BufferFindToggle::Regex)
                              : std::nullopt;
    if (toggle.has_value()) {
      operations_.toggle_buffer_search_option(*toggle);
      return true;
    }
  }
  return operations_.text_input_handle_single_line_key_down(event, modifiers);
}

bool KeyInputCoordinator::HandleOverlayKeyDown(const SDL_KeyboardEvent& event,
                                               SDL_Keymod modifiers) {
  const std::optional<int> nav_delta = ListNavigationDelta(event.key);
  const bool is_enter = event.key == SDLK_RETURN || event.key == SDLK_KP_ENTER;

  if (state_.overlay.mode == OverlayMode::CommitPicker) {
    if (event.key == SDLK_ESCAPE) {
      operations_.dismiss_overlay(false);
      return true;
    }
    if (is_enter) {
      operations_.activate_overlay_selection();
      return true;
    }
    if (nav_delta.has_value()) {
      operations_.move_compare_picker_selection(*nav_delta);
      return true;
    }
    return operations_.text_input_handle_single_line_key_down(event, modifiers);
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
      default:
        return HandleSharedBufferSearchKey(event, modifiers);
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
      default:
        return HandleSharedBufferSearchKey(event, modifiers);
    }
  }

  if (state_.overlay.mode == OverlayMode::ProjectSearch) {
    if (event.key == SDLK_ESCAPE) {
      operations_.dismiss_overlay(true);
      return true;
    }
    if (is_enter) {
      operations_.activate_overlay_selection();
      return true;
    }
    if (nav_delta.has_value()) {
      operations_.move_project_search_selection(*nav_delta);
      return true;
    }
    return operations_.text_input_handle_single_line_key_down(event, modifiers);
  }

  if (state_.overlay.mode == OverlayMode::CommandPalette) {
    auto& palette = state_.overlay.workflow.command_palette;
    if (event.key == SDLK_ESCAPE) {
      operations_.dismiss_overlay(false);
      return true;
    }
    if (is_enter) {
      operations_.activate_overlay_selection();
      return true;
    }
    if (event.key == SDLK_TAB) {
      // The palette query doubles as a command line; Tab completes the active token.
      operations_.complete_command_palette_query();
      return true;
    }
    if (nav_delta.has_value()) {
      MoveOverlayListSelection(palette.selected_index, palette.matches.size(), *nav_delta);
      return true;
    }
    return operations_.text_input_handle_single_line_key_down(event, modifiers);
  }

  if (state_.overlay.mode == OverlayMode::LaunchConfigPicker) {
    auto& picker = state_.overlay.workflow.launch_config_picker;
    if (event.key == SDLK_ESCAPE) {
      operations_.dismiss_overlay(false);
      return true;
    }
    if (is_enter) {
      operations_.activate_overlay_selection();
      return true;
    }
    if (nav_delta.has_value()) {
      MoveOverlayListSelection(picker.selected_index, picker.matches.size(), *nav_delta);
      return true;
    }
    return operations_.text_input_handle_single_line_key_down(event, modifiers);
  }

  if (state_.overlay.mode == OverlayMode::Completion ||
      state_.overlay.mode == OverlayMode::CodeActions) {
    // Completion and code-actions are the same list widget over different item vectors;
    // bind the active list once and drive it through the shared clamped-move helper.
    // These two carry no query field, so they are the only overlays where Home/End
    // address the list rather than a text caret.
    const bool is_completion = state_.overlay.mode == OverlayMode::Completion;
    const std::size_t item_count = is_completion
                                       ? state_.overlay.workflow.completion.items.size()
                                       : state_.overlay.workflow.code_actions.items.size();
    std::size_t& selected_index = is_completion
                                      ? state_.overlay.workflow.completion.selected_index
                                      : state_.overlay.workflow.code_actions.selected_index;
    if (event.key == SDLK_ESCAPE) {
      operations_.dismiss_overlay(false);
      return true;
    }
    if (is_enter) {
      operations_.activate_overlay_selection();
      return true;
    }
    if (nav_delta.has_value()) {
      MoveOverlayListSelection(selected_index, item_count, *nav_delta);
      return true;
    }
    if (event.key == SDLK_HOME || event.key == SDLK_END) {
      const int to_end = static_cast<int>(std::min<std::size_t>(
          item_count, static_cast<std::size_t>(std::numeric_limits<int>::max())));
      MoveOverlayListSelection(selected_index, item_count,
                               event.key == SDLK_HOME ? -to_end : to_end);
      return true;
    }
    return false;
  }

  // File finder (the default overlay list).
  if (is_enter) {
    operations_.activate_overlay_selection();
    return true;
  }
  if (nav_delta.has_value()) {
    operations_.move_file_finder_selection(*nav_delta);
    return true;
  }
  return operations_.text_input_handle_single_line_key_down(event, modifiers);
}

}  // namespace microide::workspace
