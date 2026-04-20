#include "workspace/WorkspaceKeyInputCoordinator.h"

#include <limits>

namespace microide::workspace {

bool KeyInputCoordinator::HandleCompareKeyDown(const SDL_KeyboardEvent& event,
                                               SDL_Keymod modifiers) {
  CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab != nullptr && compare_tab->right_editable && compare_tab->right_view_active) {
    auto& viewport = compare_tab->right_viewport;
    const auto apply_compare_edit = [&](auto&& edit) {
      const bool was_dirty = viewport.dirty();
      const std::size_t cursor_before_line = viewport.cursor_line();
      const std::vector<std::string> before_lines = viewport.lines();
      edit();
      operations_.refresh_compare_tab_derived_state(*compare_tab);
      operations_.sync_compare_selection_from_viewport(*compare_tab, true);
      operations_.reset_caret_blink();
      operations_.request_active_editable_change_redraw(before_lines, viewport.lines());
      if (viewport.dirty() != was_dirty) {
        operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                      viewport.cursor_line());
        operations_.request_tab_strip_redraw();
      }
      return true;
    };
    const auto sync_compare_navigation = [&](std::size_t previous_selected_row) {
      operations_.sync_compare_selection_from_viewport(*compare_tab, true);
      operations_.reset_caret_blink();
      if (compare_tab->selected_row != previous_selected_row) {
        operations_.request_compare_row_range_redraw(previous_selected_row, previous_selected_row + 1);
        operations_.request_compare_row_range_redraw(compare_tab->selected_row,
                                                     compare_tab->selected_row + 1);
      } else {
        operations_.request_focused_editor_redraw();
      }
      return true;
    };

    if ((modifiers & SDL_KMOD_ALT) != 0) {
      if (event.key == SDLK_LEFTBRACKET) {
        operations_.jump_compare_hunk(-1);
        return true;
      }
      if (event.key == SDLK_RIGHTBRACKET) {
        operations_.jump_compare_hunk(1);
        return true;
      }
      const char input_character = operations_.keycode_to_ascii(event.key, modifiers & ~SDL_KMOD_ALT);
      if (input_character == 'o') {
        operations_.open_working_file_from_compare();
        return true;
      }
    }

    switch (event.key) {
      case SDLK_ESCAPE:
        operations_.request_close_active_tab();
        return true;
      case SDLK_TAB:
        return apply_compare_edit([&]() { viewport.InsertTab(); });
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return apply_compare_edit([&]() { viewport.InsertNewline(); });
      case SDLK_BACKSPACE:
        return apply_compare_edit([&]() { viewport.Backspace(); });
      case SDLK_DELETE:
        return apply_compare_edit([&]() { viewport.DeleteForward(); });
      case SDLK_UP: {
        const std::size_t previous_selected_row = compare_tab->selected_row;
        viewport.MoveCursorVertical(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
        return sync_compare_navigation(previous_selected_row);
      }
      case SDLK_DOWN: {
        const std::size_t previous_selected_row = compare_tab->selected_row;
        viewport.MoveCursorVertical(1, (modifiers & SDL_KMOD_SHIFT) != 0);
        return sync_compare_navigation(previous_selected_row);
      }
      case SDLK_LEFT: {
        const std::size_t previous_selected_row = compare_tab->selected_row;
        viewport.MoveCursorHorizontal(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
        return sync_compare_navigation(previous_selected_row);
      }
      case SDLK_RIGHT: {
        const std::size_t previous_selected_row = compare_tab->selected_row;
        viewport.MoveCursorHorizontal(1, (modifiers & SDL_KMOD_SHIFT) != 0);
        return sync_compare_navigation(previous_selected_row);
      }
      case SDLK_PAGEUP: {
        const std::size_t previous_selected_row = compare_tab->selected_row;
        viewport.Page(-1);
        return sync_compare_navigation(previous_selected_row);
      }
      case SDLK_PAGEDOWN: {
        const std::size_t previous_selected_row = compare_tab->selected_row;
        viewport.Page(1);
        return sync_compare_navigation(previous_selected_row);
      }
      case SDLK_HOME: {
        const std::size_t previous_selected_row = compare_tab->selected_row;
        if (modifiers & SDL_KMOD_CTRL) {
          viewport.MoveCursorTo(0, 0, (modifiers & SDL_KMOD_SHIFT) != 0);
        } else {
          viewport.MoveCursorLineStart((modifiers & SDL_KMOD_SHIFT) != 0);
        }
        return sync_compare_navigation(previous_selected_row);
      }
      case SDLK_END: {
        const std::size_t previous_selected_row = compare_tab->selected_row;
        if (modifiers & SDL_KMOD_CTRL) {
          const std::size_t last_line = viewport.line_count() == 0 ? 0 : viewport.line_count() - 1;
          viewport.MoveCursorTo(last_line, std::numeric_limits<std::size_t>::max(),
                                (modifiers & SDL_KMOD_SHIFT) != 0);
        } else {
          viewport.MoveCursorLineEnd((modifiers & SDL_KMOD_SHIFT) != 0);
        }
        return sync_compare_navigation(previous_selected_row);
      }
      default:
        break;
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      operations_.request_close_active_tab();
      return true;
    case SDLK_UP:
      operations_.move_compare_selection(-1);
      return true;
    case SDLK_DOWN:
      operations_.move_compare_selection(1);
      return true;
    case SDLK_PAGEUP:
      operations_.move_compare_selection(-20);
      return true;
    case SDLK_PAGEDOWN:
      operations_.move_compare_selection(20);
      return true;
    case SDLK_HOME:
      if (auto* active_compare_tab = operations_.active_compare_tab(); active_compare_tab != nullptr) {
        const std::size_t previous_selected_row = active_compare_tab->selected_row;
        active_compare_tab->selected_row = 0;
        operations_.reveal_active_compare_selection();
        operations_.request_compare_row_range_redraw(previous_selected_row, previous_selected_row + 1);
        operations_.request_compare_row_range_redraw(active_compare_tab->selected_row,
                                                     active_compare_tab->selected_row + 1);
      }
      return true;
    case SDLK_END:
      if (auto* active_compare_tab = operations_.active_compare_tab();
          active_compare_tab != nullptr && !active_compare_tab->model.rows.empty()) {
        const std::size_t previous_selected_row = active_compare_tab->selected_row;
        active_compare_tab->selected_row = active_compare_tab->model.rows.size() - 1;
        operations_.reveal_active_compare_selection();
        operations_.request_compare_row_range_redraw(previous_selected_row, previous_selected_row + 1);
        operations_.request_compare_row_range_redraw(active_compare_tab->selected_row,
                                                     active_compare_tab->selected_row + 1);
      }
      return true;
    case SDLK_LEFTBRACKET:
      operations_.jump_compare_hunk(-1);
      return true;
    case SDLK_RIGHTBRACKET:
      operations_.jump_compare_hunk(1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      operations_.open_working_file_from_compare();
      return true;
    default: {
      const char input_character = operations_.keycode_to_ascii(event.key, modifiers);
      if (input_character == 'j') {
        operations_.move_compare_selection(1);
        return true;
      }
      if (input_character == 'k') {
        operations_.move_compare_selection(-1);
        return true;
      }
      if (input_character == 'o') {
        operations_.open_working_file_from_compare();
        return true;
      }
      return false;
    }
  }
}

bool KeyInputCoordinator::HandleMergeKeyDown(const SDL_KeyboardEvent& event,
                                             SDL_Keymod modifiers) {
  MergeTabState* merge_tab = operations_.active_merge_tab();
  if (merge_tab == nullptr) {
    return false;
  }

  auto& viewport = merge_tab->result_viewport;
  const auto apply_merge_edit = [&](auto&& edit) {
    const bool was_dirty = viewport.dirty();
    const std::size_t cursor_before_line = viewport.cursor_line();
    const std::vector<std::string> before_lines = viewport.lines();
    const std::optional<editor::SelectionRange> selection_before = viewport.selection_range();
    const editor::TextPosition cursor_before{viewport.cursor_line(), viewport.cursor_column()};
    edit();
    operations_.update_merge_tracking_after_viewport_edit(*merge_tab, before_lines,
                                                          selection_before, cursor_before);
    operations_.reset_caret_blink();
    operations_.request_active_editable_change_redraw(before_lines, viewport.lines());
    if (viewport.dirty() != was_dirty) {
      operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                    viewport.cursor_line());
      operations_.request_tab_strip_redraw();
    }
    return true;
  };
  const auto sync_merge_navigation = [&]() {
    merge_tab->scroll_row = static_cast<int>(viewport.scroll_line());
    merge_tab->horizontal_scroll = viewport.horizontal_scroll();
    operations_.reset_caret_blink();
    operations_.request_focused_editor_redraw();
    return true;
  };

  if ((modifiers & SDL_KMOD_ALT) != 0) {
    const char input_character = operations_.keycode_to_ascii(event.key, modifiers & ~SDL_KMOD_ALT);
    if (event.key == SDLK_LEFTBRACKET) {
      operations_.move_merge_selection(-1);
      return true;
    }
    if (event.key == SDLK_RIGHTBRACKET) {
      operations_.move_merge_selection(1);
      return true;
    }
    if (input_character == 'i') {
      operations_.apply_merge_choice(compare::MergeChoice::Incoming);
      return true;
    }
    if (input_character == 'c') {
      operations_.apply_merge_choice(compare::MergeChoice::Current);
      return true;
    }
    if (input_character == 'b') {
      operations_.apply_merge_choice(compare::MergeChoice::Base);
      return true;
    }
    if (input_character == 'm') {
      operations_.apply_merge_choice(compare::MergeChoice::Both);
      return true;
    }
    if (input_character == 'o') {
      operations_.open_merge_result_file();
      return true;
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      operations_.request_close_active_tab();
      return true;
    case SDLK_TAB:
      return apply_merge_edit([&]() { viewport.InsertTab(); });
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      return apply_merge_edit([&]() { viewport.InsertNewline(); });
    case SDLK_BACKSPACE:
      return apply_merge_edit([&]() { viewport.Backspace(); });
    case SDLK_DELETE:
      return apply_merge_edit([&]() { viewport.DeleteForward(); });
    case SDLK_UP:
      viewport.MoveCursorVertical(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      return sync_merge_navigation();
    case SDLK_DOWN:
      viewport.MoveCursorVertical(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      return sync_merge_navigation();
    case SDLK_LEFT:
      viewport.MoveCursorHorizontal(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      return sync_merge_navigation();
    case SDLK_RIGHT:
      viewport.MoveCursorHorizontal(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      return sync_merge_navigation();
    case SDLK_PAGEUP:
      viewport.Page(-1);
      return sync_merge_navigation();
    case SDLK_PAGEDOWN:
      viewport.Page(1);
      return sync_merge_navigation();
    case SDLK_HOME:
      if (modifiers & SDL_KMOD_CTRL) {
        viewport.MoveCursorTo(0, 0, (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        viewport.MoveCursorLineStart((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      return sync_merge_navigation();
    case SDLK_END:
      if (modifiers & SDL_KMOD_CTRL) {
        const std::size_t last_line = viewport.line_count() == 0 ? 0 : viewport.line_count() - 1;
        viewport.MoveCursorTo(last_line, std::numeric_limits<std::size_t>::max(),
                              (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        viewport.MoveCursorLineEnd((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      return sync_merge_navigation();
    default:
      return false;
  }
}

bool KeyInputCoordinator::HandleDefaultEditorKeyDown(const SDL_KeyboardEvent& event,
                                                     SDL_Keymod modifiers) {
  editor::TextViewport* viewport = operations_.active_editor_viewport();
  if (viewport == nullptr) {
    return false;
  }

  switch (event.key) {
    case SDLK_TAB: {
      const bool was_dirty = viewport->dirty();
      const std::size_t cursor_before_line = viewport->cursor_line();
      const std::vector<std::string> before_lines = viewport->lines();
      viewport->InsertTab();
      operations_.reset_caret_blink();
      operations_.request_active_editable_change_redraw(before_lines, viewport->lines());
      if (viewport->dirty() != was_dirty) {
        operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                      viewport->cursor_line());
        operations_.request_tab_strip_redraw();
      }
      return true;
    }
    case SDLK_RETURN:
    case SDLK_KP_ENTER: {
      const bool was_dirty = viewport->dirty();
      const std::size_t cursor_before_line = viewport->cursor_line();
      const std::vector<std::string> before_lines = viewport->lines();
      viewport->InsertNewline();
      operations_.reset_caret_blink();
      operations_.request_active_editable_change_redraw(before_lines, viewport->lines());
      if (viewport->dirty() != was_dirty) {
        operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                      viewport->cursor_line());
        operations_.request_tab_strip_redraw();
      }
      return true;
    }
    case SDLK_BACKSPACE: {
      const bool was_dirty = viewport->dirty();
      const std::size_t cursor_before_line = viewport->cursor_line();
      const std::vector<std::string> before_lines = viewport->lines();
      viewport->Backspace();
      operations_.reset_caret_blink();
      operations_.request_active_editable_change_redraw(before_lines, viewport->lines());
      if (viewport->dirty() != was_dirty) {
        operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                      viewport->cursor_line());
        operations_.request_tab_strip_redraw();
      }
      return true;
    }
    case SDLK_DELETE: {
      const bool was_dirty = viewport->dirty();
      const std::size_t cursor_before_line = viewport->cursor_line();
      const std::vector<std::string> before_lines = viewport->lines();
      viewport->DeleteForward();
      operations_.reset_caret_blink();
      operations_.request_active_editable_change_redraw(before_lines, viewport->lines());
      if (viewport->dirty() != was_dirty) {
        operations_.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                      viewport->cursor_line());
        operations_.request_tab_strip_redraw();
      }
      return true;
    }
    default:
      break;
  }

  switch (event.key) {
    case SDLK_UP:
      viewport->MoveCursorVertical(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      operations_.reset_caret_blink();
      return true;
    case SDLK_DOWN:
      viewport->MoveCursorVertical(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      operations_.reset_caret_blink();
      return true;
    case SDLK_LEFT:
      viewport->MoveCursorHorizontal(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      operations_.reset_caret_blink();
      return true;
    case SDLK_RIGHT:
      viewport->MoveCursorHorizontal(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      operations_.reset_caret_blink();
      return true;
    case SDLK_PAGEUP:
      viewport->Page(-1);
      operations_.reset_caret_blink();
      return true;
    case SDLK_PAGEDOWN:
      viewport->Page(1);
      operations_.reset_caret_blink();
      return true;
    case SDLK_HOME:
      if (modifiers & SDL_KMOD_CTRL) {
        viewport->MoveCursorTo(0, 0, (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        viewport->MoveCursorLineStart((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      operations_.reset_caret_blink();
      return true;
    case SDLK_END:
      if (modifiers & SDL_KMOD_CTRL) {
        const std::size_t last_line = viewport->line_count() == 0 ? 0 : viewport->line_count() - 1;
        viewport->MoveCursorTo(last_line, std::numeric_limits<std::size_t>::max(),
                               (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        viewport->MoveCursorLineEnd((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      operations_.reset_caret_blink();
      return true;
    default:
      return false;
  }
}

}  // namespace microide::workspace
