#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <vector>

#include "workspace/ListSelection.h"
#include "util/InvokeIfSet.h"
#include "workspace/services/AssistService.h"
#include "workspace/services/CompareMergeService.h"
#include "workspace/services/QuickPickService.h"
#include "workspace/coordinators/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

namespace {

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

}

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

bool KeyInputCoordinator::HandleSharedBufferSearchKey(const SDL_KeyboardEvent& event,
                                                      SDL_Keymod modifiers) {
  if (const auto delta = ListNavigationDelta(event.key); delta.has_value()) {
    operations_.move_buffer_search_selection(*delta);
    return true;
  }
  if (event.key == SDLK_H && NormalizedKeyModifiers(modifiers) == SDL_KMOD_CTRL) {
    if (state_.overlay.mode == OverlayMode::BufferSearch) {
      operations_.execute_action(ActionId::ReplaceInBuffer, {}, ActionSource::Shortcut);
    }
    state_.overlay.buffer_search_field = BufferSearchField::Replace;
    return true;
  }
  if ((event.key == SDLK_RETURN || event.key == SDLK_KP_ENTER) &&
      NormalizedKeyModifiers(modifiers) == SDL_KMOD_ALT) {
    const auto& buffer_search = state_.overlay.workflow.buffer_search;
    editor::TextViewport* viewport = operations_.active_editable_viewport
                                         ? operations_.active_editable_viewport()
                                         : nullptr;
    if (viewport == nullptr || buffer_search.matches.empty()) {
      return true;
    }
    const std::size_t primary = std::min(buffer_search.selected_index, buffer_search.matches.size() - 1);
    const editor::SelectionRange& current = buffer_search.matches[primary];
    viewport->MoveCursorTo(current.start.line, current.start.column);
    viewport->MoveCursorTo(current.end.line, current.end.column, true);
    std::vector<editor::SelectionRange> others;
    others.reserve(buffer_search.matches.size() - 1);
    for (std::size_t i = 0; i < buffer_search.matches.size(); ++i) {
      if (i != primary) {
        others.push_back(buffer_search.matches[i]);
      }
    }
    viewport->SetSecondaryCaretsWithRanges(others);
    state_.surface.focus = FocusTarget::Editor;
    util::InvokeIfSet(operations_.reset_caret_blink);
    operations_.request_frame();
    return true;
  }
  if ((modifiers & SDL_KMOD_ALT) != 0) {
    const std::optional<BufferFindToggle> toggle =
        event.key == SDLK_C   ? std::optional(BufferFindToggle::MatchCase)
        : event.key == SDLK_W ? std::optional(BufferFindToggle::WholeWord)
        : event.key == SDLK_R ? std::optional(BufferFindToggle::Regex)
        : event.key == SDLK_L ? std::optional(BufferFindToggle::InSelection)
                              : std::nullopt;
    if (toggle.has_value()) {
      operations_.toggle_buffer_search_option(*toggle);
      return true;
    }
  }
  return services_.text_input.HandleSingleLineKeyDown(event, modifiers);
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
      services_.compare_merge.interactions().MovePickerSelection(*nav_delta);
      return true;
    }
    return services_.text_input.HandleSingleLineKeyDown(event, modifiers);
  }

  if (state_.overlay.mode == OverlayMode::BufferSearch) {
    switch (event.key) {
      case SDLK_ESCAPE:
        operations_.dismiss_overlay(true);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        if ((modifiers & SDL_KMOD_ALT) != 0) {
          return HandleSharedBufferSearchKey(event, modifiers);
        }
        operations_.move_buffer_search_selection((modifiers & SDL_KMOD_SHIFT) ? -1 : 1);
        return true;
      default:
        return HandleSharedBufferSearchKey(event, modifiers);
    }
  }

  if (state_.overlay.mode == OverlayMode::BufferReplace) {
    switch (event.key) {
      case SDLK_ESCAPE:
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
        if ((modifiers & SDL_KMOD_ALT) != 0) {
          return HandleSharedBufferSearchKey(event, modifiers);
        }
        if (modifiers & SDL_KMOD_CTRL) {
          operations_.replace_all_buffer_search_matches();
        } else if (state_.overlay.buffer_search_field == BufferSearchField::Search) {
          operations_.move_buffer_search_selection((modifiers & SDL_KMOD_SHIFT) ? -1 : 1);
        } else {
          operations_.replace_current_buffer_search_match();
        }
        return true;
      default:
        return HandleSharedBufferSearchKey(event, modifiers);
    }
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
      operations_.complete_command_palette_query();
      return true;
    }
    if (nav_delta.has_value()) {
      MoveOverlayListSelection(palette.selected_index, palette.matches.size(), *nav_delta);
      return true;
    }
    return services_.text_input.HandleSingleLineKeyDown(event, modifiers);
  }

  if (state_.overlay.mode == OverlayMode::QuickPick) {
    auto& picker = state_.overlay.workflow.quick_pick;
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
      services_.quick_pick.SelectionChanged();
      return true;
    }
    return services_.text_input.HandleSingleLineKeyDown(event, modifiers);
  }

  if (state_.overlay.mode == OverlayMode::Completion ||
      state_.overlay.mode == OverlayMode::CodeActions) {
    const bool is_completion = state_.overlay.mode == OverlayMode::Completion;
    const std::size_t item_count = is_completion
                                       ? state_.overlay.workflow.completion.VisibleCount()
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
    if (!is_completion) {
      return false;
    }
    if (event.key == SDLK_TAB && (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) == 0) {
      operations_.activate_overlay_selection();
      return true;
    }
    bool handled = false;
    if (operations_.active_tab_is_compare()) {
      handled = HandleCompareKeyDown(event, modifiers);
    } else if (operations_.active_tab_is_merge()) {
      handled = HandleMergeKeyDown(event, modifiers);
    } else {
      handled = HandleDefaultEditorKeyDown(event, modifiers);
    }
    if (handled) {
      operations_.request_frame();
      services_.assist.completion().FollowCompletionCaret();
    }
    return handled;
  }

  if (is_enter) {
    operations_.activate_overlay_selection();
    return true;
  }
  if (nav_delta.has_value()) {
    operations_.move_file_finder_selection(*nav_delta);
    return true;
  }
  return services_.text_input.HandleSingleLineKeyDown(event, modifiers);
}

}
