#include "workspace/WorkspaceKeyInputCoordinator.h"

#include <algorithm>

#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

bool KeyInputCoordinator::HandleDirtyPromptKeyDown(const SDL_KeyboardEvent& event,
                                                   SDL_Keymod modifiers) {
  (void)modifiers;
  switch (event.key) {
    case SDLK_ESCAPE:
      prompts_.dirty.selected_action = 2;
      operations_.confirm_dirty_prompt();
      return true;
    case SDLK_LEFT:
      prompts_.dirty.selected_action = std::max(0, prompts_.dirty.selected_action - 1);
      return true;
    case SDLK_RIGHT:
    case SDLK_TAB:
      prompts_.dirty.selected_action = std::min(2, prompts_.dirty.selected_action + 1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      operations_.confirm_dirty_prompt();
      return true;
    default: {
      const char input_character = operations_.keycode_to_ascii(event.key, SDL_GetModState());
      if (input_character == 's') {
        prompts_.dirty.selected_action = 0;
        operations_.confirm_dirty_prompt();
        return true;
      }
      if (input_character == 'd') {
        prompts_.dirty.selected_action = 1;
        operations_.confirm_dirty_prompt();
        return true;
      }
      if (input_character == 'c') {
        prompts_.dirty.selected_action = 2;
        operations_.confirm_dirty_prompt();
        return true;
      }
      return true;
    }
  }
}

bool KeyInputCoordinator::HandleTreeContextMenuKeyDown(const SDL_KeyboardEvent& event) {
  switch (event.key) {
    case SDLK_ESCAPE:
      operations_.close_tree_context_menu();
      return true;
    case SDLK_DOWN:
      menu_state_.tree_context_menu.active_item_index = operations_.next_enabled_tree_context_menu_item_index(
          menu_state_.tree_context_menu.active_item_index, 1);
      return true;
    case SDLK_UP:
      menu_state_.tree_context_menu.active_item_index = operations_.next_enabled_tree_context_menu_item_index(
          menu_state_.tree_context_menu.active_item_index, -1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (menu_state_.tree_context_menu.active_item_index >= 0) {
        return operations_.execute_tree_context_menu_item(
            static_cast<std::size_t>(menu_state_.tree_context_menu.active_item_index));
      }
      return true;
    default:
      return true;
  }
}

bool KeyInputCoordinator::HandleMenuBarKeyDown(const SDL_KeyboardEvent& event,
                                               SDL_Keymod modifiers) {
  switch (event.key) {
    case SDLK_ESCAPE:
      operations_.close_menu_bar();
      return true;
    case SDLK_LEFT:
      return operations_.switch_menu_bar_menu(-1);
    case SDLK_RIGHT:
      return operations_.switch_menu_bar_menu(1);
    case SDLK_TAB:
      return operations_.switch_menu_bar_menu((modifiers & SDL_KMOD_SHIFT) != 0 ? -1 : 1);
    case SDLK_DOWN:
      return operations_.move_active_menu_item(1);
    case SDLK_UP:
      return operations_.move_active_menu_item(-1);
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (menu_state_.active_menu_item_index >= 0) {
        return operations_.execute_menu_item(menu_state_.active_menu_id,
                                             static_cast<std::size_t>(menu_state_.active_menu_item_index));
      }
      return true;
    default:
      return true;
  }
}

bool KeyInputCoordinator::HandlePromptSurfaceKeyDown(const SDL_KeyboardEvent& event) {
  if (prompts_.surface.kind == PromptSurfaceState::Kind::TextInput) {
    const SDL_Keymod modifiers = event.mod != SDL_KMOD_NONE ? event.mod : SDL_GetModState();
    if ((modifiers & SDL_KMOD_CTRL) != 0 && event.key == SDLK_V) {
      return operations_.execute_action(ActionId::PasteClipboard, {}, ActionSource::Shortcut);
    }
    switch (event.key) {
      case SDLK_ESCAPE:
        operations_.dismiss_prompt_surface(true);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        prompts_.surface.selected_button = 0;
        operations_.confirm_prompt_surface();
        return true;
      case SDLK_BACKSPACE:
        RemoveLastUtf8Codepoint(&prompts_.surface.input);
        return true;
      default:
        return true;
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      operations_.dismiss_prompt_surface(true);
      return true;
    case SDLK_LEFT:
      prompts_.surface.selected_button = std::max(0, prompts_.surface.selected_button - 1);
      return true;
    case SDLK_RIGHT:
    case SDLK_TAB:
      prompts_.surface.selected_button = std::min(1, prompts_.surface.selected_button + 1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      operations_.confirm_prompt_surface();
      return true;
    default:
      return true;
  }
}

}  // namespace microide::workspace
