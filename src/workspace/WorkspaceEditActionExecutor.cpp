#include "workspace/WorkspaceActionCoordinator.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "workspace/WorkspaceActionRequests.h"
#include "workspace/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

WorkspaceShell::ActionCoordinator::DispatchResult WorkspaceShell::ActionCoordinator::ExecuteEdit(
    ActionId id,
    const std::vector<std::string>& args,
    ActionSource source,
    std::string* rejection_feedback) {
  (void)source;
  (void)rejection_feedback;

  switch (id) {
    case ActionId::Goto:
    case ActionId::Jump: {
      if (shell_.ActiveTabIsCompare() || shell_.ActiveTabIsMerge()) {
        return DispatchResult::Handled;
      }
      const std::optional<LineNavigationRequest> request =
          BuildLineNavigationRequest(args, id == ActionId::Jump);
      if (!request.has_value()) {
        return DispatchResult::Handled;
      }

      if (id == ActionId::Goto && request->requested_line == 0) {
        return DispatchResult::Handled;
      }

      const std::size_t line_count = std::max<std::size_t>(1, shell_.text_viewport_.line_count());
      std::size_t line = 0;
      if (id == ActionId::Jump) {
        const long long current_line =
            static_cast<long long>(shell_.text_viewport_.cursor_line()) + 1;
        const long long target_line = current_line + request->requested_line;
        line = static_cast<std::size_t>(
            std::clamp(target_line - 1, 0LL, static_cast<long long>(line_count - 1)));
      } else if (request->requested_line > 0) {
        line = static_cast<std::size_t>(request->requested_line - 1);
      } else {
        const std::size_t from_end = static_cast<std::size_t>(-request->requested_line);
        line = from_end >= line_count ? 0 : line_count - from_end;
      }

      shell_.text_viewport_.MoveCursorTo(line, request->column > 0 ? request->column - 1 : 0);
      shell_.surface_.focus = FocusTarget::Editor;
      shell_.RequestFocusedEditorRedraw();
      return DispatchResult::Handled;
    }
    case ActionId::SelectAll:
      if (auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
        viewport->SelectAll();
        shell_.ResetCaretBlink();
        shell_.RequestFocusedEditorRedraw();
      }
      shell_.surface_.focus = FocusTarget::Editor;
      return DispatchResult::Handled;
    case ActionId::Undo:
      if (auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
        const bool was_dirty = viewport->dirty();
        const std::vector<std::string> before_lines = viewport->lines();
        const std::optional<editor::SelectionRange> selection_before =
            viewport->selection_range();
        const editor::TextPosition cursor_before{viewport->cursor_line(),
                                                 viewport->cursor_column()};
        if (viewport->Undo()) {
          if (auto* compare_tab = shell_.ActiveCompareTab();
              compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
            shell_.RefreshCompareTabDerivedState(*compare_tab);
            shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
          }
          if (auto* merge_tab = shell_.ActiveMergeTab();
              merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
            shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                        cursor_before);
          }
          shell_.ResetCaretBlink();
          shell_.RequestActiveTabRedraw(false);
          shell_.RequestFocusedEditorRedraw();
          shell_.RequestActiveEditableChangeRedraw(before_lines, viewport->lines());
          if (viewport->dirty() != was_dirty) {
            shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before.line,
                                                                viewport->cursor_line());
            shell_.RequestTabStripRedraw();
          }
        }
      }
      return DispatchResult::Handled;
    case ActionId::Redo:
      if (auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
        const bool was_dirty = viewport->dirty();
        const std::vector<std::string> before_lines = viewport->lines();
        const std::optional<editor::SelectionRange> selection_before =
            viewport->selection_range();
        const editor::TextPosition cursor_before{viewport->cursor_line(),
                                                 viewport->cursor_column()};
        if (viewport->Redo()) {
          if (auto* compare_tab = shell_.ActiveCompareTab();
              compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
            shell_.RefreshCompareTabDerivedState(*compare_tab);
            shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
          }
          if (auto* merge_tab = shell_.ActiveMergeTab();
              merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
            shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before,
                                                        cursor_before);
          }
          shell_.ResetCaretBlink();
          shell_.RequestActiveTabRedraw(false);
          shell_.RequestFocusedEditorRedraw();
          shell_.RequestActiveEditableChangeRedraw(before_lines, viewport->lines());
          if (viewport->dirty() != was_dirty) {
            shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before.line,
                                                                viewport->cursor_line());
            shell_.RequestTabStripRedraw();
          }
        }
      }
      return DispatchResult::Handled;
    case ActionId::CopySelection: {
      std::string text;
      if (shell_.surface_.focus == FocusTarget::Panel && shell_.TerminalHasSelection()) {
        text = shell_.SelectedTerminalText();
      } else if (shell_.ActiveEditableViewport() != nullptr) {
        text = shell_.ActiveEditableViewport()->SelectedText();
      }
      if (!text.empty()) {
        shell_.WriteClipboardText(text);
        shell_.WritePrimarySelectionText(text);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CopyLastTerminalCommand: {
      const std::optional<std::string> text = shell_.LastTerminalCommandText();
      if (text.has_value()) {
        shell_.WriteClipboardText(*text);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CopySelectionWithContext: {
      const std::optional<std::string> text = shell_.SelectionTextWithContext();
      if (text.has_value()) {
        shell_.WriteClipboardText(*text);
        shell_.WritePrimarySelectionText(*text);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CutSelection: {
      if (auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
        const bool was_dirty = viewport->dirty();
        const std::string text = viewport->SelectedText();
        if (!text.empty() && shell_.WriteClipboardText(text)) {
          shell_.WritePrimarySelectionText(text);
          const std::vector<std::string> before_lines = viewport->lines();
          const std::optional<editor::SelectionRange> selection_before =
              viewport->selection_range();
          const editor::TextPosition cursor_before{viewport->cursor_line(),
                                                   viewport->cursor_column()};
          viewport->DeleteSelectedText();
          if (auto* compare_tab = shell_.ActiveCompareTab();
              compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
            shell_.RefreshCompareTabDerivedState(*compare_tab);
            shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
          }
          if (auto* merge_tab = shell_.ActiveMergeTab();
              merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
            shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines,
                                                        selection_before, cursor_before);
          }
          shell_.ResetCaretBlink();
          shell_.RequestActiveTabRedraw(false);
          shell_.RequestFocusedEditorRedraw();
          shell_.RequestActiveEditableChangeRedraw(before_lines, viewport->lines());
          if (viewport->dirty() != was_dirty) {
            shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before.line,
                                                                viewport->cursor_line());
            shell_.RequestTabStripRedraw();
          }
        }
      }
      return DispatchResult::Handled;
    }
    case ActionId::PasteClipboard: {
      if (const std::optional<std::string> clipboard_text = shell_.ReadClipboardText();
          clipboard_text.has_value()) {
        if (shell_.surface_.focus == FocusTarget::Panel && shell_.ActiveTerminalTab() != nullptr) {
          TextInputCoordinator(shell_).PasteClipboardIntoTerminal();
        } else if (auto* viewport = shell_.ActiveEditableViewport(); viewport != nullptr) {
          const bool was_dirty = viewport->dirty();
          const std::vector<std::string> before_lines = viewport->lines();
          const std::optional<editor::SelectionRange> selection_before =
              viewport->selection_range();
          const editor::TextPosition cursor_before{viewport->cursor_line(),
                                                   viewport->cursor_column()};
          viewport->InsertText(*clipboard_text);
          if (auto* compare_tab = shell_.ActiveCompareTab();
              compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
            shell_.RefreshCompareTabDerivedState(*compare_tab);
            shell_.SyncCompareSelectionFromViewport(*compare_tab, true);
          }
          if (auto* merge_tab = shell_.ActiveMergeTab();
              merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
            shell_.UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines,
                                                        selection_before, cursor_before);
          }
          shell_.ResetCaretBlink();
          shell_.RequestActiveTabRedraw(false);
          shell_.RequestFocusedEditorRedraw();
          shell_.RequestActiveEditableChangeRedraw(before_lines, viewport->lines());
          if (viewport->dirty() != was_dirty) {
            shell_.RequestActiveEditableBlameNeighborhoodRedraw(cursor_before.line,
                                                                viewport->cursor_line());
            shell_.RequestTabStripRedraw();
          }
        }
      }
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
