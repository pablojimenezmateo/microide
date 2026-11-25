#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"

#include <limits>
#include <optional>

#include "editor/ShapingActions.h"
#include "util/PerformanceTrace.h"
#include "workspace/SettingFlags.h"
#include "util/InvokeIfSet.h"

namespace microide::workspace {

namespace {

bool EditorShapingLineOpsSettingEnabled(const KeyInputCoordinator::Operations& operations) {
  if (!operations.get_setting_value) {
    return true;
  }
  return SettingFlagEnabled(operations.get_setting_value("editor.shaping.line_ops.enabled"),
                            true);
}

void MoveCursorHorizontalStep(editor::TextViewport& viewport, int delta, SDL_Keymod modifiers) {
  const bool extend_selection = (modifiers & SDL_KMOD_SHIFT) != 0;
  if ((modifiers & SDL_KMOD_CTRL) != 0) {
    viewport.MoveCursorWord(delta, extend_selection);
  } else {
    viewport.MoveCursorHorizontal(delta, extend_selection);
  }
}

template <typename ApplyEdit, typename AfterNavigation>
bool HandleEditableViewportMotionAndEdit(editor::TextViewport& viewport,
                                         const SDL_KeyboardEvent& event,
                                         SDL_Keymod modifiers,
                                         const ApplyEdit& apply_edit,
                                         const AfterNavigation& after_navigation,
                                         bool* handled) {
  *handled = true;
  const bool extend_selection = (modifiers & SDL_KMOD_SHIFT) != 0;
  switch (event.key) {
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

template <typename ApplyEdit, typename AfterNavigation>
bool HandleEditablePaneKey(KeyInputCoordinator::Operations& operations,
                           editor::TextViewport& viewport,
                           const SDL_KeyboardEvent& event,
                           SDL_Keymod modifiers,
                           const ApplyEdit& apply_edit,
                           const AfterNavigation& after_navigation,
                           bool* handled) {
  if (event.key == SDLK_TAB) {
    *handled = true;
    const editor::TabKeyIntent intent =
        editor::ClassifyTabKey(viewport, (modifiers & SDL_KMOD_SHIFT) != 0);
    if (intent != editor::TabKeyIntent::kInsertTab) {
      if (!EditorShapingLineOpsSettingEnabled(operations)) {
        return true;
      }
      return apply_edit([&]() {
        switch (intent) {
          case editor::TabKeyIntent::kOutdent:
            editor::OutdentSelection(viewport);
            break;
          case editor::TabKeyIntent::kMixed:
            editor::ApplyMixedTabKey(viewport);
            break;
          default:
            editor::IndentSelection(viewport);
            break;
        }
      });
    }
    return apply_edit([&]() { viewport.InsertTab(); });
  }
  return HandleEditableViewportMotionAndEdit(viewport, event, modifiers, apply_edit,
                                             after_navigation, handled);
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
  util::InvokeIfSet(operations.mark_active_editor_folding_dirty);
  {
    util::PerformanceTrace::Scope scope(
        "KeyInputCoordinator::HandleDefaultEditorKeyDown::RequestLastChangeRedraw");
    operations.note_active_editable_edit();
  }
  if (viewport.cursor_line() != cursor_before_line) {
    operations.request_frame();
  }
  if (viewport.dirty() != was_dirty) {
    util::PerformanceTrace::Scope scope(
        "KeyInputCoordinator::HandleDefaultEditorKeyDown::DirtyStateSideEffects");
    operations.request_frame();
  }
  return true;
}

}

bool KeyInputCoordinator::HandleCompareKeyDown(const SDL_KeyboardEvent& event,
                                               SDL_Keymod modifiers) {
  CompareTabState* compare_tab = operations_.active_compare_tab();
  if (compare_tab != nullptr && compare_tab->right_view_active) {
    auto& viewport = compare_tab->right_viewport;
    const auto apply_compare_edit = [&](auto&& edit) {
      if (!compare_tab->right_editable) {
        return true;
      }
      edit();
      operations_.refresh_compare_tab_derived_state(*compare_tab);
      operations_.sync_compare_selection_from_viewport(*compare_tab, true);
      operations_.reset_caret_blink();
      operations_.note_active_editable_edit();
      return true;
    };
    const auto sync_compare_navigation = [&]() {
      operations_.sync_compare_selection_from_viewport(*compare_tab, true);
      operations_.reset_caret_blink();
      operations_.request_frame();
      return true;
    };

    if ((modifiers & SDL_KMOD_ALT) != 0) {
      const char input_character = operations_.keycode_to_ascii(event.key, modifiers & ~SDL_KMOD_ALT);
      if (input_character == 'o') {
        operations_.open_working_file_from_compare();
        return true;
      }
    }

    if (event.key == SDLK_ESCAPE) {
      operations_.request_close_active_tab();
      return true;
    }

    bool handled_by_shared_switch = false;
    const bool result = HandleEditablePaneKey(
        operations_, viewport, event, modifiers, apply_compare_edit,
        [&]() { return sync_compare_navigation(); },
        &handled_by_shared_switch);
    if (handled_by_shared_switch) {
      return result;
    }
    if (compare_tab->right_editable &&
        (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 &&
        operations_.keycode_to_ascii(event.key, modifiers) != '\0') {
      return false;
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
        active_compare_tab->selected_row = 0;
        operations_.reveal_active_compare_selection();
        operations_.request_frame();
      }
      return true;
    case SDLK_END:
      if (auto* active_compare_tab = operations_.active_compare_tab();
          active_compare_tab != nullptr && !active_compare_tab->model.rows.empty()) {
        active_compare_tab->selected_row = active_compare_tab->model.rows.size() - 1;
        operations_.reveal_active_compare_selection();
        operations_.request_frame();
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
    edit();
    operations_.update_merge_tracking_after_viewport_edit(*merge_tab);
    operations_.reset_caret_blink();
    operations_.note_active_editable_edit();
    return true;
  };
  const auto sync_merge_navigation = [&]() {
    merge_tab->scroll_row = static_cast<int>(viewport.scroll_line());
    merge_tab->horizontal_scroll = viewport.horizontal_scroll();
    operations_.reset_caret_blink();
    operations_.request_frame();
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
    if (input_character == 'r' && operations_.execute_action) {
      return operations_.execute_action(ActionId::MergeResetConflict, {}, ActionSource::Shortcut);
    }
  }

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
  editor::TextViewport& viewport = state_.sidebar.git.commit_workflow.body;
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
        util::InvokeIfSet(operations_.commit_body_write_clipboard_text, selection);
        viewport.DeleteSelectedText();
        ClearCommitBodyHint(viewport);
        return after_edit();
      }
      case SDLK_V: {
        if (operations_.commit_body_read_clipboard_text) {
          if (const auto pasted = operations_.commit_body_read_clipboard_text();
              pasted.has_value() && !pasted->empty()) {
            ClearCommitBodyHint(viewport);
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

  bool handled = false;
  const bool result = HandleEditableViewportMotionAndEdit(
      viewport, event, modifiers,
      [&](const auto& edit) {
        ClearCommitBodyHint(viewport);
        edit();
        return after_edit();
      },
      after_edit, &handled);
  if (handled) {
    return result;
  }
  return operations_.keycode_to_ascii != nullptr &&
         operations_.keycode_to_ascii(event.key, modifiers) != '\0';
}

bool KeyInputCoordinator::HandleDefaultEditorKeyDown(const SDL_KeyboardEvent& event,
                                                     SDL_Keymod modifiers) {
  editor::TextViewport* viewport = operations_.active_editor_viewport();
  editor::TextViewport* editable_viewport = operations_.active_editable_viewport();
  if (viewport == nullptr) {
    return false;
  }

  if (event.key == SDLK_ESCAPE) {
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
    if (viewport->has_multiple_carets()) {
      viewport->ClearSecondaryCarets();
      operations_.reset_caret_blink();
      operations_.request_frame();
      return true;
    }
    if (viewport->has_selection()) {
      viewport->ClearSelection();
      operations_.reset_caret_blink();
      operations_.request_frame();
      return true;
    }
    return false;
  }
  if (event.key == SDLK_TAB && operations_.accept_inline_completion()) {
    return true;
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
      const editor::TabKeyIntent intent =
          editor::ClassifyTabKey(*editable_viewport, (modifiers & SDL_KMOD_SHIFT) != 0);
      if (intent == editor::TabKeyIntent::kOutdent) {
        if (!EditorShapingLineOpsSettingEnabled(operations_)) {
          return true;
        }
        return ApplyDefaultEditorEdit(
            operations_, *editable_viewport,
            "KeyInputCoordinator::HandleDefaultEditorKeyDown::OutdentSelection",
            [&]() { editor::OutdentSelection(*editable_viewport); });
      }
      if (intent == editor::TabKeyIntent::kIndentBlock) {
        if (!EditorShapingLineOpsSettingEnabled(operations_)) {
          return true;
        }
        return ApplyDefaultEditorEdit(
            operations_, *editable_viewport,
            "KeyInputCoordinator::HandleDefaultEditorKeyDown::IndentSelection",
            [&]() { editor::IndentSelection(*editable_viewport); });
      }
      if (intent == editor::TabKeyIntent::kMixed) {
        if (!EditorShapingLineOpsSettingEnabled(operations_)) {
          return true;
        }
        return ApplyDefaultEditorEdit(
            operations_, *editable_viewport,
            "KeyInputCoordinator::HandleDefaultEditorKeyDown::MixedTabKey",
            [&]() { editor::ApplyMixedTabKey(*editable_viewport); });
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
    util::InvokeIfSet(operations_.notify_snippet_session_caret_moved);
  };

  const bool ctrl_only =
      (modifiers & SDL_KMOD_CTRL) != 0 && (modifiers & (SDL_KMOD_SHIFT | SDL_KMOD_ALT)) == 0;
  switch (event.key) {
    case SDLK_UP:
      if (ctrl_only) {
        const std::size_t scroll = viewport->scroll_line();
        viewport->SetScrollLine(scroll > 0 ? scroll - 1 : 0);
        return true;
      }
      viewport->MoveCursorVertical(-1, (modifiers & SDL_KMOD_SHIFT) != 0);
      after_editor_caret_motion();
      return true;
    case SDLK_DOWN:
      if (ctrl_only) {
        viewport->SetScrollLine(viewport->scroll_line() + 1);
        return true;
      }
      viewport->MoveCursorVertical(1, (modifiers & SDL_KMOD_SHIFT) != 0);
      after_editor_caret_motion();
      return true;
    case SDLK_L: {
      if (!ctrl_only) {
        return false;
      }
      viewport->ExpandSelectionToWholeLines();
      after_editor_caret_motion();
      return true;
    }
    case SDLK_LEFT:
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

}
