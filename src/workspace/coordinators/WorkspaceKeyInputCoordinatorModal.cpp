#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"

#include <algorithm>
#include <optional>

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
    // Home/End were the two keys the menus did not answer, in a shell where every
    // other list does. They cannot go through ListNavigationKeyDelta: that resolver
    // expresses them as a ±count delta, which lands on the ends only for a mover
    // that clamps — this one wraps, so ±count is a full lap back to where it
    // started. Stepping once from "no selection" is the primitive's own way of
    // naming the first/last enabled item, skipping separators and disabled rows.
    case SDLK_HOME:
      menu_state_.tree_context_menu.active_item_index =
          operations_.next_enabled_tree_context_menu_item_index(-1, 1);
      return true;
    case SDLK_END:
      menu_state_.tree_context_menu.active_item_index =
          operations_.next_enabled_tree_context_menu_item_index(-1, -1);
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
    // See the tree context menu above for why these step from "no selection"
    // rather than by a ±count delta.
    case SDLK_HOME:
    case SDLK_END:
      menu_state_.active_menu_item_index = -1;
      return operations_.move_active_menu_item(event.key == SDLK_HOME ? 1 : -1);
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

// The menu-bar overflow popup, which holds the menus that did not fit in compact
// layout. Mirrors HandleMenuBarKeyDown: every key is consumed while the popup is
// up so nothing leaks into the surface underneath, and Esc is what closes it.
bool KeyInputCoordinator::HandleMenuOverflowPopupKeyDown(const SDL_KeyboardEvent& event,
                                                         SDL_Keymod modifiers) {
  const std::size_t item_count = operations_.menu_overflow_item_count();
  if (item_count == 0) {
    operations_.close_menu_overflow_popup();
    return true;
  }
  const int last_index = static_cast<int>(item_count) - 1;
  // Every row of this popup is an enabled menu, so stepping is plain arithmetic
  // rather than the enabled-item search the popup menus need. It wraps, as the
  // popup menus do.
  const auto step = [&](int delta) {
    const int current = menu_state_.overflow_popup_active_index;
    int next = current < 0 ? (delta > 0 ? 0 : last_index) : current + delta;
    if (next < 0) {
      next = last_index;
    } else if (next > last_index) {
      next = 0;
    }
    menu_state_.overflow_popup_active_index = next;
    return true;
  };

  switch (event.key) {
    case SDLK_ESCAPE:
      operations_.close_menu_overflow_popup();
      return true;
    case SDLK_DOWN:
      return step(1);
    case SDLK_UP:
      return step(-1);
    case SDLK_TAB:
      return step((modifiers & SDL_KMOD_SHIFT) != 0 ? -1 : 1);
    case SDLK_HOME:
      menu_state_.overflow_popup_active_index = 0;
      return true;
    case SDLK_END:
      menu_state_.overflow_popup_active_index = last_index;
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE:
    // Right opens the highlighted menu, matching the submenu gesture in the
    // popup menus: the overflow row IS a menu, so "open what is selected".
    case SDLK_RIGHT:
      if (menu_state_.overflow_popup_active_index >= 0) {
        operations_.open_menu_overflow_item(
            static_cast<std::size_t>(menu_state_.overflow_popup_active_index));
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
      case SDLK_DELETE:
      case SDLK_LEFT:
      case SDLK_RIGHT:
      case SDLK_HOME:
      case SDLK_END:
        return operations_.text_input_handle_single_line_key_down(event, modifiers);
      default:
        return operations_.text_input_handle_single_line_key_down(event, modifiers) || true;
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
      prompts_.surface.selected_button =
          std::min(std::max(0, prompts_.surface.button_count - 1),
                   prompts_.surface.selected_button + 1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      operations_.confirm_prompt_surface();
      return true;
    default:
      return true;
  }
}

bool KeyInputCoordinator::HandleNotificationFocusKeyDown(const SDL_KeyboardEvent& event,
                                                         SDL_Keymod modifiers) {
  if (!notifications_.HasFocus()) {
    return false;
  }
  using FocusKey = NotificationService::FocusKey;
  std::optional<FocusKey> key;
  switch (event.key) {
    case SDLK_UP: key = FocusKey::Older; break;
    case SDLK_DOWN: key = FocusKey::Newer; break;
    case SDLK_LEFT: key = FocusKey::Previous; break;
    case SDLK_RIGHT: key = FocusKey::Next; break;
    case SDLK_TAB:
      key = (modifiers & SDL_KMOD_SHIFT) != 0 ? FocusKey::Previous : FocusKey::Next;
      break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE: key = FocusKey::Activate; break;
    case SDLK_DELETE: key = FocusKey::Close; break;
    case SDLK_ESCAPE: key = FocusKey::Leave; break;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
    case SDLK_LCTRL:
    case SDLK_RCTRL:
    case SDLK_LALT:
    case SDLK_RALT:
    case SDLK_LGUI:
    case SDLK_RGUI:
      return true;  // a bare modifier is the start of a chord, not a reason to leave
    default:
      break;
  }
  if (!key.has_value()) {
    // VS Code gives focus back on any other key and lets it through, so typing
    // after glancing at a toast still types.
    notifications_.ClearFocus();
    EnsureRedraw([this]() { operations_.request_window_redraw(); });
    return false;
  }
  NotificationService::FocusKeyResult result = notifications_.HandleFocusKey(*key);
  if (result.action.has_value()) {
    operations_.execute_action(result.action->id, result.action->args, ActionSource::Command);
  }
  if (result.changed) {
    EnsureRedraw([this]() { operations_.request_window_redraw(); });
  }
  return true;
}

}  // namespace microide::workspace
