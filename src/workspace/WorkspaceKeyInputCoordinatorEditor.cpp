#include "workspace/WorkspaceKeyInputCoordinator.h"

#include <limits>

namespace microide::workspace {

bool KeyInputCoordinator::HandleCompareKeyDown(const SDL_KeyboardEvent& event,
                                               SDL_Keymod modifiers) {
  WorkspaceShell::CompareTabState* compare_tab = shell_.ActiveCompareTab();
  if (compare_tab != nullptr && compare_tab->right_editable && compare_tab->right_view_active) {
    auto& viewport = compare_tab->right_viewport;
    const auto apply_compare_edit = [&](auto&& edit) {
      const bool was_dirty = viewport.dirty();
      const std::size_t cursor_before_line = viewport.cursor_line();
      const std::vector<std::string> before_lines = viewport.lines();
      edit();
      shell_.RefreshCompareTabDerivedState(*compare_tab);
      shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
      shell_.ResetCaretBlink();
      shell_.RequestActiveEditableChangeRedraw(before_lines, viewport.lines());
      if (viewport.dirty() != was_dirty) {
        shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line,
                                                            viewport.cursor_line());
        shell_.RequestTabStripRedraw();
      }
      return true;
    };
    const auto sync_compare_navigation = [&](std::size_t previous_selected_row) {
      shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
      shell_.ResetCaretBlink();
      if (compare_tab->selected_row != previous_selected_row) {
        shell_.RequestCompareRowRangeRedraw(previous_selected_row, previous_selected_row + 1);
        shell_.RequestCompareRowRangeRedraw(compare_tab->selected_row,
                                            compare_tab->selected_row + 1);
      } else {
        shell_.RequestFocusedEditorRedraw();
      }
      return true;
    };

    if ((modifiers & SDL_KMOD_ALT) != 0) {
      if (event.key == SDLK_LEFTBRACKET) {
        shell_.JumpCompareHunk(-1);
        return true;
      }
      if (event.key == SDLK_RIGHTBRACKET) {
        shell_.JumpCompareHunk(1);
        return true;
      }
      const char input_character = shell_.KeycodeToAscii(event.key, modifiers & ~SDL_KMOD_ALT);
      if (input_character == 'o') {
        shell_.OpenWorkingFileFromCompare();
        return true;
      }
    }

    switch (event.key) {
      case SDLK_ESCAPE:
        shell_.RequestCloseTab(shell_.active_tab_index_);
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
      shell_.RequestCloseTab(shell_.active_tab_index_);
      return true;
    case SDLK_UP:
      shell_.MoveCompareSelection(-1);
      return true;
    case SDLK_DOWN:
      shell_.MoveCompareSelection(1);
      return true;
    case SDLK_PAGEUP:
      shell_.MoveCompareSelection(-20);
      return true;
    case SDLK_PAGEDOWN:
      shell_.MoveCompareSelection(20);
      return true;
    case SDLK_HOME:
      if (auto* active_compare_tab = shell_.ActiveCompareTab(); active_compare_tab != nullptr) {
        const std::size_t previous_selected_row = active_compare_tab->selected_row;
        active_compare_tab->selected_row = 0;
        shell_.RevealActiveCompareSelection();
        shell_.RequestCompareRowRangeRedraw(previous_selected_row, previous_selected_row + 1);
        shell_.RequestCompareRowRangeRedraw(active_compare_tab->selected_row,
                                            active_compare_tab->selected_row + 1);
      }
      return true;
    case SDLK_END:
      if (auto* active_compare_tab = shell_.ActiveCompareTab();
          active_compare_tab != nullptr && !active_compare_tab->model.rows.empty()) {
        const std::size_t previous_selected_row = active_compare_tab->selected_row;
        active_compare_tab->selected_row = active_compare_tab->model.rows.size() - 1;
        shell_.RevealActiveCompareSelection();
        shell_.RequestCompareRowRangeRedraw(previous_selected_row, previous_selected_row + 1);
        shell_.RequestCompareRowRangeRedraw(active_compare_tab->selected_row,
                                            active_compare_tab->selected_row + 1);
      }
      return true;
    case SDLK_LEFTBRACKET:
      shell_.JumpCompareHunk(-1);
      return true;
    case SDLK_RIGHTBRACKET:
      shell_.JumpCompareHunk(1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      shell_.OpenWorkingFileFromCompare();
      return true;
    default: {
      const char input_character = shell_.KeycodeToAscii(event.key, modifiers);
      if (input_character == 'j') {
        shell_.MoveCompareSelection(1);
        return true;
      }
      if (input_character == 'k') {
        shell_.MoveCompareSelection(-1);
        return true;
      }
      if (input_character == 'o') {
        shell_.OpenWorkingFileFromCompare();
        return true;
      }
      return false;
    }
  }
}

bool KeyInputCoordinator::HandleMergeKeyDown(const SDL_KeyboardEvent& event,
                                             SDL_Keymod modifiers) {
  WorkspaceShell::MergeTabState* merge_tab = shell_.ActiveMergeTab();
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
    shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                cursor_before);
    shell_.ResetCaretBlink();
    shell_.RequestActiveEditableChangeRedraw(before_lines, viewport.lines());
    if (viewport.dirty() != was_dirty) {
      shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line,
                                                          viewport.cursor_line());
      shell_.RequestTabStripRedraw();
    }
    return true;
  };
  const auto sync_merge_navigation = [&]() {
    merge_tab->scroll_row = static_cast<int>(viewport.scroll_line());
    merge_tab->horizontal_scroll = viewport.horizontal_scroll();
    shell_.ResetCaretBlink();
    shell_.RequestFocusedEditorRedraw();
    return true;
  };

  if ((modifiers & SDL_KMOD_ALT) != 0) {
    const char input_character = shell_.KeycodeToAscii(event.key, modifiers & ~SDL_KMOD_ALT);
    if (event.key == SDLK_LEFTBRACKET) {
      shell_.MoveMergeSelection(-1);
      return true;
    }
    if (event.key == SDLK_RIGHTBRACKET) {
      shell_.MoveMergeSelection(1);
      return true;
    }
    if (input_character == 'i') {
      shell_.ApplyMergeChoice(compare::MergeChoice::Incoming);
      return true;
    }
    if (input_character == 'c') {
      shell_.ApplyMergeChoice(compare::MergeChoice::Current);
      return true;
    }
    if (input_character == 'b') {
      shell_.ApplyMergeChoice(compare::MergeChoice::Base);
      return true;
    }
    if (input_character == 'm') {
      shell_.ApplyMergeChoice(compare::MergeChoice::Both);
      return true;
    }
    if (input_character == 'o') {
      shell_.OpenMergeResultFile();
      return true;
    }
  }

  switch (event.key) {
    case SDLK_ESCAPE:
      shell_.RequestCloseTab(shell_.active_tab_index_);
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
  switch (event.key) {
    case SDLK_TAB: {
      const bool was_dirty = shell_.text_viewport_.dirty();
      const std::size_t cursor_before_line = shell_.text_viewport_.cursor_line();
      const std::vector<std::string> before_lines = shell_.text_viewport_.lines();
      shell_.text_viewport_.InsertTab();
      shell_.ResetCaretBlink();
      shell_.RequestActiveEditableChangeRedraw(before_lines, shell_.text_viewport_.lines());
      if (shell_.text_viewport_.dirty() != was_dirty) {
        shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line,
                                                            shell_.text_viewport_.cursor_line());
        shell_.RequestTabStripRedraw();
      }
      return true;
    }
    case SDLK_RETURN:
    case SDLK_KP_ENTER: {
      const bool was_dirty = shell_.text_viewport_.dirty();
      const std::size_t cursor_before_line = shell_.text_viewport_.cursor_line();
      const std::vector<std::string> before_lines = shell_.text_viewport_.lines();
      shell_.text_viewport_.InsertNewline();
      shell_.ResetCaretBlink();
      shell_.RequestActiveEditableChangeRedraw(before_lines, shell_.text_viewport_.lines());
      if (shell_.text_viewport_.dirty() != was_dirty) {
        shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line,
                                                            shell_.text_viewport_.cursor_line());
        shell_.RequestTabStripRedraw();
      }
      return true;
    }
    case SDLK_BACKSPACE: {
      const bool was_dirty = shell_.text_viewport_.dirty();
      const std::size_t cursor_before_line = shell_.text_viewport_.cursor_line();
      const std::vector<std::string> before_lines = shell_.text_viewport_.lines();
      shell_.text_viewport_.Backspace();
      shell_.ResetCaretBlink();
      shell_.RequestActiveEditableChangeRedraw(before_lines, shell_.text_viewport_.lines());
      if (shell_.text_viewport_.dirty() != was_dirty) {
        shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line,
                                                            shell_.text_viewport_.cursor_line());
        shell_.RequestTabStripRedraw();
      }
      return true;
    }
    case SDLK_DELETE: {
      const bool was_dirty = shell_.text_viewport_.dirty();
      const std::size_t cursor_before_line = shell_.text_viewport_.cursor_line();
      const std::vector<std::string> before_lines = shell_.text_viewport_.lines();
      shell_.text_viewport_.DeleteForward();
      shell_.ResetCaretBlink();
      shell_.RequestActiveEditableChangeRedraw(before_lines, shell_.text_viewport_.lines());
      if (shell_.text_viewport_.dirty() != was_dirty) {
        shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before_line,
                                                            shell_.text_viewport_.cursor_line());
        shell_.RequestTabStripRedraw();
      }
      return true;
    }
    default:
      break;
  }

  switch (event.key) {
    case SDLK_UP:
      shell_.text_viewport_.MoveCursorVertical(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      shell_.ResetCaretBlink();
      return true;
    case SDLK_DOWN:
      shell_.text_viewport_.MoveCursorVertical(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      shell_.ResetCaretBlink();
      return true;
    case SDLK_LEFT:
      shell_.text_viewport_.MoveCursorHorizontal(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      shell_.ResetCaretBlink();
      return true;
    case SDLK_RIGHT:
      shell_.text_viewport_.MoveCursorHorizontal(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      shell_.ResetCaretBlink();
      return true;
    case SDLK_PAGEUP:
      shell_.text_viewport_.Page(-1);
      shell_.ResetCaretBlink();
      return true;
    case SDLK_PAGEDOWN:
      shell_.text_viewport_.Page(1);
      shell_.ResetCaretBlink();
      return true;
    case SDLK_HOME:
      if (modifiers & SDL_KMOD_CTRL) {
        shell_.text_viewport_.MoveCursorTo(0, 0, (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        shell_.text_viewport_.MoveCursorLineStart((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      shell_.ResetCaretBlink();
      return true;
    case SDLK_END:
      if (modifiers & SDL_KMOD_CTRL) {
        const std::size_t last_line =
            shell_.text_viewport_.line_count() == 0 ? 0 : shell_.text_viewport_.line_count() - 1;
        shell_.text_viewport_.MoveCursorTo(last_line, std::numeric_limits<std::size_t>::max(),
                                           (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        shell_.text_viewport_.MoveCursorLineEnd((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      shell_.ResetCaretBlink();
      return true;
    default:
      return false;
  }
}

}  // namespace microide::workspace
