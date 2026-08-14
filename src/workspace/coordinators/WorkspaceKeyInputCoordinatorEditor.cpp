#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"

#include <limits>
#include <optional>

#include "editor/ShapingActions.h"
#include "util/PerformanceTrace.h"
#include "workspace/SettingFlags.h"

namespace microide::workspace {

namespace {

bool EditorShapingLineOpsSettingEnabled(const KeyInputCoordinator::Operations& operations) {
  if (!operations.get_setting_value) {
    return true;
  }
  return SettingFlagEnabled(operations.get_setting_value("editor.shaping.line_ops.enabled"),
                            /*default_value=*/true);
}

// One horizontal caret step for every multi-line text surface in the shell: the
// editor, the compare and merge panes, and the git commit body. Ctrl makes the
// step word-granular (VS Code's cursorWordStartLeft / cursorWordEndRight) and
// Shift extends, in either granularity.
//
// It is a shared helper rather than four copies of the same `if` because the
// four surfaces had already drifted once — three of them had no word step at
// all, and neither did the editor.
void MoveCursorHorizontalStep(editor::TextViewport& viewport, int delta, SDL_Keymod modifiers) {
  const bool extend_selection = (modifiers & SDL_KMOD_SHIFT) != 0;
  if ((modifiers & SDL_KMOD_CTRL) != 0) {
    viewport.MoveCursorWord(delta, extend_selection);
  } else {
    viewport.MoveCursorHorizontal(delta, extend_selection);
  }
}

// The raw key switch shared by every editable text surface that is NOT the editor
// pane: the compare right pane and the merge result pane. Both run the same
// `editor::TextViewport` and reach the same action layer, but each used to carry
// its own hand-maintained copy of this switch — which is how the compare pane
// ended up without Shift+Tab outdent or Tab-indents-a-multi-line-selection, with
// no error and no test to fail (TD-2026-08-13-207).
//
// `apply_edit` runs the mutation plus that surface's post-edit refresh (compare
// rebuilds its diff model, merge re-syncs its conflict state); `after_navigation`
// is the surface's post-move selection sync. Returns false for a key it does not
// own, so the caller keeps its surface-specific arms (Esc, the Alt chords).
template <typename ApplyEdit, typename AfterNavigation>
bool HandleEditablePaneKey(KeyInputCoordinator::Operations& operations,
                           editor::TextViewport& viewport,
                           const SDL_KeyboardEvent& event,
                           SDL_Keymod modifiers,
                           const ApplyEdit& apply_edit,
                           const AfterNavigation& after_navigation,
                           bool* handled) {
  *handled = true;
  const bool extend_selection = (modifiers & SDL_KMOD_SHIFT) != 0;
  // Any key that is not a column-select step ends the box gesture, so the next
  // Ctrl+Shift+Alt+Arrow re-anchors where the caret ended up rather than
  // extending a stale box. The editor pane has done this since column selection
  // shipped; the diff panes never did, even though the ColumnSelect* actions
  // resolve through ActiveEditableViewport() and therefore reach them
  // (TD-2026-08-13-207). The steps themselves arrive as actions and are
  // dispatched before this handler runs.
  viewport.ClearColumnSelection();
  switch (event.key) {
    case SDLK_TAB: {
      // Same three-way split as the editor pane: Shift outdents, a multi-line
      // selection indents as a block, and a bare Tab inserts one.
      const bool shift_held = (modifiers & SDL_KMOD_SHIFT) != 0;
      const auto selection = viewport.selection_range();
      const bool multi_line_selection =
          selection.has_value() && selection->start.line != selection->end.line;
      if (shift_held || multi_line_selection) {
        if (!EditorShapingLineOpsSettingEnabled(operations)) {
          return true;
        }
        return apply_edit([&]() {
          if (shift_held) {
            editor::OutdentSelection(viewport);
          } else {
            editor::IndentSelection(viewport);
          }
        });
      }
      return apply_edit([&]() { viewport.InsertTab(); });
    }
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      return apply_edit([&]() { viewport.InsertNewline(); });
    case SDLK_BACKSPACE:
      return apply_edit([&]() {
        if ((modifiers & SDL_KMOD_CTRL) != 0) {
          viewport.DeleteWord(-1);
        } else {
          viewport.Backspace();
        }
      });
    case SDLK_DELETE:
      return apply_edit([&]() {
        if ((modifiers & SDL_KMOD_CTRL) != 0) {
          viewport.DeleteWord(1);
        } else {
          viewport.DeleteForward();
        }
      });
    case SDLK_UP:
      viewport.MoveCursorVertical(-1, extend_selection);
      return after_navigation();
    case SDLK_DOWN:
      viewport.MoveCursorVertical(1, extend_selection);
      return after_navigation();
    case SDLK_LEFT:
      MoveCursorHorizontalStep(viewport, -1, modifiers);
      return after_navigation();
    case SDLK_RIGHT:
      MoveCursorHorizontalStep(viewport, 1, modifiers);
      return after_navigation();
    case SDLK_PAGEUP:
      viewport.Page(-1, extend_selection);
      return after_navigation();
    case SDLK_PAGEDOWN:
      viewport.Page(1, extend_selection);
      return after_navigation();
    case SDLK_HOME:
      if ((modifiers & SDL_KMOD_CTRL) != 0) {
        viewport.JumpCursorTo(0, 0, extend_selection);
      } else {
        viewport.MoveCursorLineStart(extend_selection);
      }
      return after_navigation();
    case SDLK_END:
      if ((modifiers & SDL_KMOD_CTRL) != 0) {
        const std::size_t last_line = viewport.line_count() == 0 ? 0 : viewport.line_count() - 1;
        viewport.JumpCursorTo(last_line, std::numeric_limits<std::size_t>::max(), extend_selection);
      } else {
        viewport.MoveCursorLineEnd(extend_selection);
      }
      return after_navigation();
    default:
      break;
  }
  *handled = false;
  return false;
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

    // Esc closes the compare tab rather than collapsing carets: a deliberate
    // divergence from the editor pane, and the reason this path keeps an arm of
    // its own in front of the shared switch.
    if (event.key == SDLK_ESCAPE) {
      operations_.request_close_active_tab();
      return true;
    }

    const std::size_t selected_row_before_key = compare_tab->selected_row;
    bool handled_by_shared_switch = false;
    const bool result = HandleEditablePaneKey(
        operations_, viewport, event, modifiers, apply_compare_edit,
        [&]() { return sync_compare_navigation(selected_row_before_key); },
        &handled_by_shared_switch);
    if (handled_by_shared_switch) {
      return result;
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

  // Esc closes the merge tab, as on the compare surface.
  if (event.key == SDLK_ESCAPE) {
    operations_.request_close_active_tab();
    return true;
  }

  bool handled_by_shared_switch = false;
  const bool result = HandleEditablePaneKey(operations_, viewport, event, modifiers,
                                            apply_merge_edit, sync_merge_navigation,
                                            &handled_by_shared_switch);
  return handled_by_shared_switch && result;
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
      if ((modifiers & SDL_KMOD_CTRL) != 0) {
        viewport.DeleteWord(-1);
      } else {
        viewport.Backspace();
      }
      return after_edit();
    case SDLK_DELETE:
      if ((modifiers & SDL_KMOD_CTRL) != 0) {
        viewport.DeleteWord(1);
      } else {
        viewport.DeleteForward();
      }
      return after_edit();
    case SDLK_UP:
      viewport.MoveCursorVertical(-1, extend_selection);
      return after_edit();
    case SDLK_DOWN:
      viewport.MoveCursorVertical(1, extend_selection);
      return after_edit();
    case SDLK_LEFT:
      MoveCursorHorizontalStep(viewport, -1, modifiers);
      return after_edit();
    case SDLK_RIGHT:
      MoveCursorHorizontalStep(viewport, 1, modifiers);
      return after_edit();
    case SDLK_PAGEUP:
      viewport.Page(-1, extend_selection);
      return after_edit();
    case SDLK_PAGEDOWN:
      viewport.Page(1, extend_selection);
      return after_edit();
    case SDLK_HOME:
      if (modifiers & SDL_KMOD_CTRL) {
        viewport.JumpCursorTo(0, 0, extend_selection);
      } else {
        viewport.MoveCursorLineStart(extend_selection);
      }
      return after_edit();
    case SDLK_END:
      if (modifiers & SDL_KMOD_CTRL) {
        const std::size_t last_line = viewport.line_count() == 0 ? 0 : viewport.line_count() - 1;
        viewport.JumpCursorTo(last_line, std::numeric_limits<std::size_t>::max(), extend_selection);
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
    // Esc collapses a multi-caret set back to the primary caret (VS Code's
    // `removeSecondaryCursors`). Without it the only way out of a Ctrl+D run was
    // to click somewhere, and a stray caret left behind edits at a place the user
    // has stopped looking at. Deliberately after the widget/snippet/completion
    // arms, which are what Esc means while any of them is up, and deliberately
    // NOT clearing the selection: Esc removes cursors, it does not deselect.
    // `viewport`, not `editable_viewport`: removing carets is not an edit, and a
    // read-only buffer holds a Ctrl+D caret set just as well as a writable one.
    if (viewport->has_multiple_carets()) {
      viewport->ClearSecondaryCarets();
      operations_.reset_caret_blink();
      operations_.request_focused_editor_redraw();
      return true;
    }
    return false;
  }
  if (event.key == SDLK_TAB && operations_.accept_inline_completion()) {
    return true;
  }

  // Any editor key that is not a column-select step ends the gesture, so the next
  // Ctrl+Shift+Alt+Arrow re-anchors at wherever the caret ended up instead of
  // extending a stale box. The steps themselves arrive through the keybinding
  // registry as ColumnSelect* actions and are dispatched before this handler runs.
  if (editable_viewport != nullptr) {
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
      // Ctrl+Backspace deletes the word to the left, as everywhere else. The
      // single-line surfaces have had this since they shipped; the main editor
      // used to fall through here and delete one character.
      if ((modifiers & SDL_KMOD_CTRL) != 0) {
        return ApplyDefaultEditorEdit(
            operations_, *editable_viewport,
            "KeyInputCoordinator::HandleDefaultEditorKeyDown::DeleteWordBackward",
            [&]() { editable_viewport->DeleteWord(-1); });
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
      if ((modifiers & SDL_KMOD_CTRL) != 0) {
        return ApplyDefaultEditorEdit(
            operations_, *editable_viewport,
            "KeyInputCoordinator::HandleDefaultEditorKeyDown::DeleteWordForward",
            [&]() { editable_viewport->DeleteWord(1); });
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
      // Ctrl steps a word at a time. The column-select chord
      // Ctrl+Shift+Alt+Left is dispatched by the keybinding registry before this
      // handler runs, so Alt never reaches here holding Ctrl.
      MoveCursorHorizontalStep(*viewport, -1, modifiers);
      after_editor_caret_motion();
      return true;
    case SDLK_RIGHT:
      MoveCursorHorizontalStep(*viewport, 1, modifiers);
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
        // A jump to the other end of the DOCUMENT collapses to one caret, as in
        // VS Code: it would otherwise strand carets a screenful away from the one
        // that just moved.
        viewport->JumpCursorTo(0, 0, (modifiers & SDL_KMOD_SHIFT) != 0);
      } else {
        viewport->MoveCursorLineStart((modifiers & SDL_KMOD_SHIFT) != 0);
      }
      after_editor_caret_motion();
      return true;
    case SDLK_END:
      if (modifiers & SDL_KMOD_CTRL) {
        const std::size_t last_line = viewport->line_count() == 0 ? 0 : viewport->line_count() - 1;
        viewport->JumpCursorTo(last_line, std::numeric_limits<std::size_t>::max(),
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
