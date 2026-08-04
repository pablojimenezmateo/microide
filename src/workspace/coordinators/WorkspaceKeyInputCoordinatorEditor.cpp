#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"

#include <limits>
#include <optional>

#include "editor/ColumnSelection.h"
#include "editor/ShapingActions.h"
#include "util/PerformanceTrace.h"
#include "workspace/SettingFlags.h"

namespace microide::workspace {

namespace {

// Keyboard column (box) selection: VSCode's Ctrl+Shift+Alt+Arrow.
//
// Mouse box selection already existed (Shift+Alt+drag, WorkspaceEditorMouseCoordinator)
// and TextViewport::SetBoxSelection already did the hard part -- per-line column
// clamping and the 10,000-caret span cap. The keyboard half was missing entirely, so
// without a pointer there was no way to make a rectangular selection.
//
// One helper rather than a copy per editable surface: the plain editor, the compare
// panes and the merge result pane are all TextViewports and all take the same chord.
// Returns true when the event was a column-select step and was consumed.
bool TryColumnSelectStep(editor::TextViewport& viewport,
                         const SDL_KeyboardEvent& event,
                         SDL_Keymod modifiers) {
  const bool chord = (modifiers & SDL_KMOD_CTRL) != 0 && (modifiers & SDL_KMOD_SHIFT) != 0 &&
                     (modifiers & SDL_KMOD_ALT) != 0;
  if (!chord) {
    return false;
  }
  std::optional<editor::ColumnSelectDirection> direction;
  switch (event.key) {
    case SDLK_UP: direction = editor::ColumnSelectDirection::Up; break;
    case SDLK_DOWN: direction = editor::ColumnSelectDirection::Down; break;
    case SDLK_LEFT: direction = editor::ColumnSelectDirection::Left; break;
    case SDLK_RIGHT: direction = editor::ColumnSelectDirection::Right; break;
    default: return false;
  }

  const editor::TextPosition caret{viewport.cursor_line(), viewport.cursor_column()};
  const editor::ColumnSelectionState before = viewport.column_selection();
  const std::size_t lo = before.active ? std::min(before.anchor.line, before.cursor.line)
                                       : caret.line;
  const std::size_t hi = before.active ? std::max(before.anchor.line, before.cursor.line)
                                       : caret.line;
  // The virtual column may only grow to the longest line the box currently covers;
  // unbounded growth would let Right run forever over a document of short lines.
  const std::size_t max_column = viewport.MaxLineLengthInSpan(lo, hi);
  const editor::ColumnSelectionState after = editor::StepColumnSelection(
      before, *direction, caret, viewport.line_count(), max_column);
  viewport.SetColumnSelection(after);
  viewport.SetBoxSelection(after.anchor, after.cursor);
  return true;
}

bool EditorShapingLineOpsSettingEnabled(const KeyInputCoordinator::Operations& operations) {
  if (!operations.get_setting_value) {
    return true;
  }
  return SettingFlagEnabled(operations.get_setting_value("editor.shaping.line_ops.enabled"),
                            /*default_value=*/true);
}

template <typename EditFn>
bool ApplyDefaultEditorEdit(KeyInputCoordinator::Operations& operations,
                            editor::TextViewport& viewport,
                            std::string_view scope_label,
                            EditFn&& edit) {
  util::PerformanceTrace::Scope edit_scope(scope_label);
  const bool was_dirty = viewport.dirty();
  const std::size_t cursor_before_line = viewport.cursor_line();
  {
    util::PerformanceTrace::Scope scope(
        "KeyInputCoordinator::HandleDefaultEditorKeyDown::ViewportEdit");
    edit();
  }
  {
    util::PerformanceTrace::Scope scope(
        "KeyInputCoordinator::HandleDefaultEditorKeyDown::ResetCaretBlink");
    operations.reset_caret_blink();
  }
  if (operations.mark_active_editor_folding_dirty) {
    operations.mark_active_editor_folding_dirty();
  }
  {
    util::PerformanceTrace::Scope scope(
        "KeyInputCoordinator::HandleDefaultEditorKeyDown::RequestLastChangeRedraw");
    operations.request_active_editable_last_change_redraw();
  }
  if (viewport.dirty() != was_dirty) {
    util::PerformanceTrace::Scope scope(
        "KeyInputCoordinator::HandleDefaultEditorKeyDown::DirtyStateSideEffects");
    operations.request_active_editable_blame_neighborhood_redraw(cursor_before_line,
                                                                 viewport.cursor_line());
    operations.request_tab_strip_redraw();
  }
  return true;
}

}  // namespace

bool KeyInputCoordinator::HandleCompareKeyDown(const SDL_KeyboardEvent& event,
                                               SDL_Keymod modifiers) {
  CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab != nullptr && compare_tab->right_view_active) {
    auto& viewport = compare_tab->right_viewport;
    const auto apply_compare_edit = [&](auto&& edit) {
      if (!compare_tab->right_editable) {
        return true;
      }
      const bool was_dirty = viewport.dirty();
      const std::size_t cursor_before_line = viewport.cursor_line();
      edit();
      operations_.refresh_compare_tab_derived_state(*compare_tab);
      operations_.sync_compare_selection_from_viewport(*compare_tab, true);
      operations_.reset_caret_blink();
      operations_.request_active_editable_last_change_redraw();
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
        viewport.Page(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
        return sync_compare_navigation(previous_selected_row);
      }
      case SDLK_PAGEDOWN: {
        const std::size_t previous_selected_row = compare_tab->selected_row;
        viewport.Page(1, (modifiers & SDL_KMOD_SHIFT) != 0);
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
      // Only bare / Shift-modified printable keys drive these single-letter compare
      // actions. An UNBOUND Ctrl+letter (e.g. Ctrl+O — the outline binding is Ctrl+Alt+O,
      // not plain Ctrl+O) misses HandleGlobalKeyDown and falls through here; KeycodeToAscii
      // ignores Ctrl/GUI and returns the base letter, so without this guard Ctrl+O would
      // fire "open working file" and Ctrl+J would scroll the diff selection.
      if ((modifiers & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0) {
        return false;
      }
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
      if (input_character == 'a') {
        operations_.stage_compare_hunk();
        return true;
      }
      if (input_character == 'A') {
        operations_.stage_compare_selected_lines();
        return true;
      }
      if (input_character == 'c') {
        operations_.unstage_compare_hunk();
        return true;
      }
      if (input_character == 'C') {
        operations_.unstage_compare_selected_lines();
        return true;
      }
      if (input_character == 'd') {
        operations_.open_discard_compare_hunk_prompt();
        return true;
      }
      if (input_character == 'D') {
        operations_.open_discard_compare_selected_lines_prompt();
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
    const std::optional<editor::SelectionRange> selection_before = viewport.selection_range();
    const editor::TextPosition cursor_before{viewport.cursor_line(), viewport.cursor_column()};
    edit();
    operations_.update_merge_tracking_after_viewport_edit(*merge_tab, selection_before,
                                                          cursor_before);
    operations_.reset_caret_blink();
    operations_.request_active_editable_last_change_redraw();
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
    if (input_character == '1') {
      operations_.apply_merge_choice(compare::MergeChoice::BothCurrentFirst);
      return true;
    }
    if (input_character == '2') {
      operations_.apply_merge_choice(compare::MergeChoice::BothIncomingFirst);
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
      viewport.Page(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      return sync_merge_navigation();
    case SDLK_PAGEDOWN:
      viewport.Page(1, (modifiers & SDL_KMOD_SHIFT) != 0);
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

bool KeyInputCoordinator::HandleCommitBodyKeyDown(const SDL_KeyboardEvent& event,
                                                  SDL_Keymod modifiers) {
  // The commit body is a self-contained multi-line field living in the git sidebar. It
  // deliberately does NOT route through ActiveEditableViewport()/HandleDefaultEditorKeyDown
  // (which are tab-bound and feed LSP/folding/blame) — it edits its own viewport directly,
  // mirroring the compare/merge per-viewport handlers.
  editor::TextViewport& viewport = state_.sidebar.git.commit_workflow.body;
  const bool extend_selection = (modifiers & SDL_KMOD_SHIFT) != 0;
  const auto after_edit = [&]() {
    operations_.reset_caret_blink();
    operations_.request_sidebar_redraw();
    return true;
  };

  if ((modifiers & SDL_KMOD_CTRL) != 0) {
    switch (event.key) {
      case SDLK_A:
        viewport.SelectAll();
        return after_edit();
      case SDLK_C: {
        const std::string selection = viewport.SelectedText();
        if (!selection.empty() && operations_.commit_body_write_clipboard_text) {
          operations_.commit_body_write_clipboard_text(selection);
        }
        return true;
      }
      case SDLK_X: {
        const std::string selection = viewport.SelectedText();
        if (selection.empty()) {
          return true;
        }
        if (operations_.commit_body_write_clipboard_text) {
          operations_.commit_body_write_clipboard_text(selection);
        }
        viewport.DeleteSelectedText();
        return after_edit();
      }
      case SDLK_V: {
        if (operations_.commit_body_read_clipboard_text) {
          if (const auto pasted = operations_.commit_body_read_clipboard_text();
              pasted.has_value() && !pasted->empty()) {
            viewport.InsertText(*pasted);
            return after_edit();
          }
        }
        return true;
      }
      default:
        break;
    }
  }

  switch (event.key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      viewport.InsertNewline();
      return after_edit();
    case SDLK_BACKSPACE:
      viewport.Backspace();
      return after_edit();
    case SDLK_DELETE:
      viewport.DeleteForward();
      return after_edit();
    case SDLK_UP:
      viewport.MoveCursorVertical(-1, extend_selection);
      return after_edit();
    case SDLK_DOWN:
      viewport.MoveCursorVertical(1, extend_selection);
      return after_edit();
    case SDLK_LEFT:
      viewport.MoveCursorHorizontal(-1, extend_selection);
      return after_edit();
    case SDLK_RIGHT:
      viewport.MoveCursorHorizontal(1, extend_selection);
      return after_edit();
    case SDLK_PAGEUP:
      viewport.Page(-1, extend_selection);
      return after_edit();
    case SDLK_PAGEDOWN:
      viewport.Page(1, extend_selection);
      return after_edit();
    case SDLK_HOME:
      if (modifiers & SDL_KMOD_CTRL) {
        viewport.MoveCursorTo(0, 0, extend_selection);
      } else {
        viewport.MoveCursorLineStart(extend_selection);
      }
      return after_edit();
    case SDLK_END:
      if (modifiers & SDL_KMOD_CTRL) {
        const std::size_t last_line = viewport.line_count() == 0 ? 0 : viewport.line_count() - 1;
        viewport.MoveCursorTo(last_line, std::numeric_limits<std::size_t>::max(), extend_selection);
      } else {
        viewport.MoveCursorLineEnd(extend_selection);
      }
      return after_edit();
    default:
      // Plain character keys are inserted by the SDL_TextInput event; consume the keydown
      // so it cannot fall through to a git-sidebar action.
      return operations_.keycode_to_ascii != nullptr &&
             operations_.keycode_to_ascii(event.key, modifiers) != '\0';
  }
}

bool KeyInputCoordinator::HandleDefaultEditorKeyDown(const SDL_KeyboardEvent& event,
                                                     SDL_Keymod modifiers) {
  editor::TextViewport* viewport = operations_.active_editor_viewport();
  editor::TextViewport* editable_viewport = operations_.active_editable_viewport();
  if (viewport == nullptr) {
    return false;
  }

  if (event.key == SDLK_ESCAPE) {
    // The find/replace widget is non-modal, so it can be open while the editor
    // holds focus; Esc closes it first (VSCode behavior).
    if (state_.overlay.visible &&
        (state_.overlay.mode == OverlayMode::BufferSearch ||
         state_.overlay.mode == OverlayMode::BufferReplace)) {
      operations_.dismiss_overlay(true);
      return true;
    }
    if (operations_.try_snippet_escape_in_editor && operations_.try_snippet_escape_in_editor()) {
      return true;
    }
    operations_.dismiss_inline_completion();
    return false;
  }
  if (event.key == SDLK_TAB && operations_.accept_inline_completion()) {
    return true;
  }

  if (editable_viewport != nullptr) {
    if (TryColumnSelectStep(*editable_viewport, event, modifiers)) {
      operations_.reset_caret_blink();
      operations_.request_window_redraw();
      return true;
    }
    // Any other editor key ends the gesture, so the next Ctrl+Shift+Alt+Arrow
    // re-anchors at wherever the caret ended up instead of extending a stale box.
    editable_viewport->ClearColumnSelection();
  }

  switch (event.key) {
    case SDLK_TAB: {
      if (editable_viewport == nullptr) {
        return true;
      }
      if (operations_.try_snippet_tab_in_editor &&
          operations_.try_snippet_tab_in_editor((modifiers & SDL_KMOD_SHIFT) != 0)) {
        return true;
      }
      const bool shift_held = (modifiers & SDL_KMOD_SHIFT) != 0;
      const auto selection = editable_viewport->selection_range();
      const bool multi_line_selection =
          selection.has_value() && selection->start.line != selection->end.line;
      if (shift_held) {
        if (!EditorShapingLineOpsSettingEnabled(operations_)) {
          return true;
        }
        return ApplyDefaultEditorEdit(
            operations_, *editable_viewport,
            "KeyInputCoordinator::HandleDefaultEditorKeyDown::OutdentSelection",
            [&]() { editor::OutdentSelection(*editable_viewport); });
      }
      if (multi_line_selection) {
        if (!EditorShapingLineOpsSettingEnabled(operations_)) {
          return true;
        }
        return ApplyDefaultEditorEdit(
            operations_, *editable_viewport,
            "KeyInputCoordinator::HandleDefaultEditorKeyDown::IndentSelection",
            [&]() { editor::IndentSelection(*editable_viewport); });
      }
      return ApplyDefaultEditorEdit(operations_, *editable_viewport,
                                    "KeyInputCoordinator::HandleDefaultEditorKeyDown::InsertTab",
                                    [&]() { editable_viewport->InsertTab(); });
    }
    case SDLK_RETURN:
    case SDLK_KP_ENTER: {
      if (editable_viewport == nullptr) {
        return true;
      }
      return ApplyDefaultEditorEdit(
          operations_, *editable_viewport,
          "KeyInputCoordinator::HandleDefaultEditorKeyDown::InsertNewline",
          [&]() { editable_viewport->InsertNewline(); });
    }
    case SDLK_BACKSPACE: {
      if (editable_viewport == nullptr) {
        return true;
      }
      if (operations_.try_snippet_backspace_in_editor &&
          operations_.try_snippet_backspace_in_editor(editable_viewport)) {
        return true;
      }
      return ApplyDefaultEditorEdit(operations_, *editable_viewport,
                                    "KeyInputCoordinator::HandleDefaultEditorKeyDown::Backspace",
                                    [&]() { editable_viewport->Backspace(); });
    }
    case SDLK_DELETE: {
      if (editable_viewport == nullptr) {
        return true;
      }
      if (operations_.try_snippet_delete_forward_in_editor &&
          operations_.try_snippet_delete_forward_in_editor(editable_viewport)) {
        return true;
      }
      return ApplyDefaultEditorEdit(
          operations_, *editable_viewport,
          "KeyInputCoordinator::HandleDefaultEditorKeyDown::DeleteForward",
          [&]() { editable_viewport->DeleteForward(); });
    }
    default:
      break;
  }

  const auto after_editor_caret_motion = [this]() {
    operations_.reset_caret_blink();
    if (operations_.notify_snippet_session_caret_moved) {
      operations_.notify_snippet_session_caret_moved();
    }
  };

  switch (event.key) {
    case SDLK_UP:
      viewport->MoveCursorVertical(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      after_editor_caret_motion();
      return true;
    case SDLK_DOWN:
      viewport->MoveCursorVertical(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      after_editor_caret_motion();
      return true;
    case SDLK_LEFT:
      viewport->MoveCursorHorizontal(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      after_editor_caret_motion();
      return true;
    case SDLK_RIGHT:
      viewport->MoveCursorHorizontal(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      after_editor_caret_motion();
      return true;
    case SDLK_PAGEUP:
      viewport->Page(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      after_editor_caret_motion();
      return true;
    case SDLK_PAGEDOWN:
      viewport->Page(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      after_editor_caret_motion();
      return true;
    case SDLK_HOME:
      if (modifiers & SDL_KMOD_CTRL) {
        viewport->MoveCursorTo(0, 0, (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        viewport->MoveCursorLineStart((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      after_editor_caret_motion();
      return true;
    case SDLK_END:
      if (modifiers & SDL_KMOD_CTRL) {
        const std::size_t last_line = viewport->line_count() == 0 ? 0 : viewport->line_count() - 1;
        viewport->MoveCursorTo(last_line, std::numeric_limits<std::size_t>::max(),
                               (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        viewport->MoveCursorLineEnd((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      after_editor_caret_motion();
      return true;
    default:
      return false;
  }
}

}  // namespace microide::workspace
