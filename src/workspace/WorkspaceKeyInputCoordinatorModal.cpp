#include "workspace/WorkspaceKeyInputCoordinator.h"

#include <algorithm>

#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

bool WorkspaceShell::KeyInputCoordinator::HandleDirtyPromptKeyDown(const SDL_KeyboardEvent& event,
                                                                   SDL_Keymod modifiers) {
  (void)modifiers;
  switch (event.key) {
    case SDLK_ESCAPE:
      shell_.prompts_.dirty.selected_action = 2;
      shell_.ConfirmDirtyPrompt();
      return true;
    case SDLK_LEFT:
      shell_.prompts_.dirty.selected_action = std::max(0, shell_.prompts_.dirty.selected_action - 1);
      return true;
    case SDLK_RIGHT:
    case SDLK_TAB:
      shell_.prompts_.dirty.selected_action = std::min(2, shell_.prompts_.dirty.selected_action + 1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      shell_.ConfirmDirtyPrompt();
      return true;
    default: {
      const char input_character = shell_.KeycodeToAscii(event.key, SDL_GetModState());
      if (input_character == 's') {
        shell_.prompts_.dirty.selected_action = 0;
        shell_.ConfirmDirtyPrompt();
        return true;
      }
      if (input_character == 'd') {
        shell_.prompts_.dirty.selected_action = 1;
        shell_.ConfirmDirtyPrompt();
        return true;
      }
      if (input_character == 'c') {
        shell_.prompts_.dirty.selected_action = 2;
        shell_.ConfirmDirtyPrompt();
        return true;
      }
      return true;
    }
  }
}

bool WorkspaceShell::KeyInputCoordinator::HandleTreeContextMenuKeyDown(
    const SDL_KeyboardEvent& event) {
  MenuCoordinator menu(shell_);
  switch (event.key) {
    case SDLK_ESCAPE:
      menu.CloseTreeContextMenu();
      return true;
    case SDLK_DOWN:
      shell_.surface_.tree_context_menu.active_item_index = menu.NextEnabledTreeContextMenuItemIndex(
          shell_.surface_.tree_context_menu.active_item_index, 1);
      return true;
    case SDLK_UP:
      shell_.surface_.tree_context_menu.active_item_index = menu.NextEnabledTreeContextMenuItemIndex(
          shell_.surface_.tree_context_menu.active_item_index, -1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (shell_.surface_.tree_context_menu.active_item_index >= 0) {
        return menu.ExecuteTreeContextMenuItem(
            static_cast<std::size_t>(shell_.surface_.tree_context_menu.active_item_index));
      }
      return true;
    default:
      return true;
  }
}

bool WorkspaceShell::KeyInputCoordinator::HandleMenuBarKeyDown(const SDL_KeyboardEvent& event,
                                                               SDL_Keymod modifiers) {
  MenuCoordinator menu(shell_);
  switch (event.key) {
    case SDLK_ESCAPE:
      menu.CloseMenuBar();
      return true;
    case SDLK_LEFT:
      return menu.SwitchMenuBarMenu(-1);
    case SDLK_RIGHT:
      return menu.SwitchMenuBarMenu(1);
    case SDLK_TAB:
      return menu.SwitchMenuBarMenu((modifiers & SDL_KMOD_SHIFT) != 0 ? -1 : 1);
    case SDLK_DOWN:
      return menu.MoveActiveMenuItem(1);
    case SDLK_UP:
      return menu.MoveActiveMenuItem(-1);
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (shell_.surface_.active_menu_item_index >= 0) {
        return menu.ExecuteMenuItem(shell_.surface_.active_menu_id,
                                    static_cast<std::size_t>(shell_.surface_.active_menu_item_index));
      }
      return true;
    default:
      return true;
  }
}

bool WorkspaceShell::KeyInputCoordinator::HandlePromptSurfaceKeyDown(const SDL_KeyboardEvent& event) {
  if (shell_.prompts_.surface.kind == PromptSurfaceState::Kind::TextInput) {
    switch (event.key) {
      case SDLK_ESCAPE:
        shell_.DismissPromptSurface(true);
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        shell_.prompts_.surface.selected_button = 0;
        shell_.ConfirmPromptSurface();
        return true;
      case SDLK_BACKSPACE:
        RemoveLastUtf8Codepoint(&shell_.prompts_.surface.input);
        return true;
      default:
        return true;
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      shell_.DismissPromptSurface(true);
      return true;
    case SDLK_LEFT:
      shell_.prompts_.surface.selected_button =
          std::max(0, shell_.prompts_.surface.selected_button - 1);
      return true;
    case SDLK_RIGHT:
    case SDLK_TAB:
      shell_.prompts_.surface.selected_button =
          std::min(1, shell_.prompts_.surface.selected_button + 1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      shell_.ConfirmPromptSurface();
      return true;
    default:
      return true;
  }
}

}  // namespace microide::workspace
