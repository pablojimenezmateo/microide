#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"

#include <optional>

#include "workspace/ListSelection.h"

namespace microide::workspace {

bool KeyInputCoordinator::HandleSettingsOverlayKeyDown(const SDL_KeyboardEvent& event,
                                                       SDL_Keymod modifiers) {
  const auto overlay_redraw = [&]() {
    EnsureRedraw([this]() { operations_.request_overlay_redraw(); });
  };

  if (operations_.settings_value_edit_active && operations_.settings_value_edit_active()) {
    const bool picker =
        operations_.settings_value_edit_is_picker && operations_.settings_value_edit_is_picker();
    if (event.key == SDLK_RETURN || event.key == SDLK_KP_ENTER) {
      operations_.settings_commit_value_edit();
    } else if (event.key == SDLK_ESCAPE) {
      operations_.settings_cancel_value_edit();
    } else if (picker && (event.key == SDLK_DOWN || event.key == SDLK_UP)) {
      operations_.settings_picker_move(event.key == SDLK_DOWN ? 1 : -1);
    } else {
      operations_.text_input_handle_single_line_key_down(event, modifiers);
      if (picker && operations_.settings_picker_reset_highlight) {
        operations_.settings_picker_reset_highlight();
      }
    }
    overlay_redraw();
    return true;
  }

  if (event.key == SDLK_ESCAPE) {
    operations_.close_settings_overlay();
    EnsureRedraw([this]() { operations_.request_window_redraw(); });
    return true;
  }

  constexpr int kFilterPane = 0;
  constexpr int kCategoryPane = 1;
  constexpr int kValuePane = 2;

  if (!operations_.settings_overlay_is_settings_mode()) {
    if (const auto delta =
            ListNavigationKeyDelta(event.key, operations_.settings_pane_item_count(kValuePane));
        delta.has_value()) {
      operations_.settings_scroll_rows(*delta);
      overlay_redraw();
      return true;
    }
    operations_.text_input_handle_single_line_key_down(event, modifiers);
    overlay_redraw();
    return true;
  }

  if (event.key == SDLK_TAB) {
    operations_.settings_cycle_focus((modifiers & SDL_KMOD_SHIFT) != 0 ? -1 : 1);
    overlay_redraw();
    return true;
  }

  const int pane = operations_.settings_focused_pane();
  if (pane == kFilterPane) {
    if (event.key == SDLK_DOWN || event.key == SDLK_UP) {
      operations_.settings_focus_pane(kValuePane);
      operations_.settings_move_row(event.key == SDLK_DOWN ? 1 : -1);
    } else {
      operations_.text_input_handle_single_line_key_down(event, modifiers);
    }
    overlay_redraw();
    return true;
  }

  if (pane == kCategoryPane) {
    if (const auto delta =
            ListNavigationKeyDelta(event.key, operations_.settings_pane_item_count(kCategoryPane));
        delta.has_value()) {
      operations_.settings_move_category(*delta);
      overlay_redraw();
      return true;
    }
    switch (event.key) {
      case SDLK_RIGHT:
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        operations_.settings_focus_pane(kValuePane);
        break;
      case SDLK_LEFT:
        operations_.settings_focus_pane(kFilterPane);
        break;
      default:
        break;
    }
    overlay_redraw();
    return true;
  }

  if (const auto delta =
          ListNavigationKeyDelta(event.key, operations_.settings_pane_item_count(kValuePane));
      delta.has_value()) {
    operations_.settings_move_row(*delta);
    overlay_redraw();
    return true;
  }
  switch (event.key) {
    case SDLK_RIGHT:
    case SDLK_EQUALS:
    case SDLK_PLUS:
    case SDLK_KP_PLUS:
      operations_.settings_step_selected(1);
      break;
    case SDLK_LEFT:
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
      operations_.settings_step_selected(-1);
      break;
    case SDLK_SPACE:
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      operations_.settings_toggle_or_activate_selected();
      break;
    case SDLK_DELETE:
      operations_.settings_reset_selected();
      break;
    default:
      break;
  }
  overlay_redraw();
  return true;
}

}
