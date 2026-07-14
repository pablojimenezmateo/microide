#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceActionServices.h"

#include <string>
#include <utility>
#include <vector>

#include "editor/BracketScanner.h"
#include "editor/ShapingActions.h"
#include "editor/TextViewport.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceActionRequests.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

bool SettingEnabled(const WorkspaceActionContext& context, std::string_view id, bool default_value) {
  return SettingFlagEnabled(context.GetSettingValue(id), default_value);
}

}  // namespace

ActionCoordinator::DispatchResult ActionCoordinator::ExecuteEdit(ActionId id,
                                                                 const std::vector<std::string>& args,
                                                                 ActionSource source,
                                                                 std::string* rejection_feedback) {
  (void)source;
  const auto reject = [&](std::string feedback) {
    if (rejection_feedback != nullptr) {
      *rejection_feedback = std::move(feedback);
    }
    return DispatchResult::Rejected;
  };

  switch (id) {
    case ActionId::Completion: {
      std::string error_message;
      if (!context_.ShowCompletionOverlay(&error_message)) {
        return reject(error_message.empty() ? "No completions available" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::InsertSnippet: {
      std::string error_message;
      if (!context_.ShowInsertSnippetOverlay(&error_message)) {
        return reject(error_message.empty() ? "No snippets available" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CodeActions: {
      std::string error_message;
      if (!context_.ShowCodeActionsOverlay(&error_message)) {
        return reject(error_message.empty() ? "No code actions available" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::FormatDocument: {
      std::string error_message;
      if (!context_.FormatActiveDocument(&error_message)) {
        return reject(error_message.empty() ? "Formatting unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::RenameSymbol: {
      context_.OpenRenameSymbolPrompt();
      return DispatchResult::Handled;
    }
    case ActionId::GoToDefinition: {
      std::string error_message;
      if (!context_.GoToLspDefinition(&error_message)) {
        return reject(error_message.empty() ? "Definition unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::GoToTypeDefinition: {
      std::string error_message;
      if (!context_.GoToLspTypeDefinition(&error_message)) {
        return reject(error_message.empty() ? "Type definition unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::GoToImplementation: {
      std::string error_message;
      if (!context_.GoToLspImplementation(&error_message)) {
        return reject(error_message.empty() ? "Implementation unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::GoToDeclaration: {
      std::string error_message;
      if (!context_.GoToLspDeclaration(&error_message)) {
        return reject(error_message.empty() ? "Declaration unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::FindReferences: {
      std::string error_message;
      if (!context_.FindLspReferences(&error_message)) {
        return reject(error_message.empty() ? "References unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::WorkspaceSymbol: {
      // The query is the joined command arguments: `workspace-symbol <query>`.
      std::string query;
      for (const std::string& arg : args) {
        if (!query.empty()) query += ' ';
        query += arg;
      }
      if (query.empty()) {
        return reject("Usage: workspace-symbol <query>");
      }
      std::string error_message;
      if (!context_.ShowWorkspaceSymbols(query, &error_message)) {
        return reject(error_message.empty() ? "Workspace symbols unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::SignatureHelp: {
      std::string error_message;
      if (!context_.ShowSignatureHelp(&error_message)) {
        return reject(error_message.empty() ? "No signature help available" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::Goto:
    case ActionId::Jump: {
      if (context_.ActiveTabIsCompare() || context_.ActiveTabIsMerge()) {
        return DispatchResult::Handled;
      }
      const std::optional<LineNavigationRequest> request =
          BuildLineNavigationRequest(args, id == ActionId::Jump);
      if (!request.has_value()) {
        // No line supplied (the Ctrl+G shortcut or the "Go to Line…" menu):
        // open the single-line "Go to Line" modal. Typing `goto <line>` in the
        // command palette still works for the keyboard-driven path.
        if (id == ActionId::Goto && args.empty()) {
          context_.OpenGoToLinePrompt();
        }
        return DispatchResult::Handled;
      }

      if (id == ActionId::Goto && request->requested_line == 0) {
        return DispatchResult::Handled;
      }

      context_.ExecuteLineNavigation(*request, id == ActionId::Jump);
      return DispatchResult::Handled;
    }
    case ActionId::SelectAll:
      context_.SelectAll();
      return DispatchResult::Handled;
    case ActionId::Undo:
      context_.Undo();
      return DispatchResult::Handled;
    case ActionId::Redo:
      context_.Redo();
      return DispatchResult::Handled;
    case ActionId::CopySelection: {
      const std::string text = context_.CopySelectionText();
      if (!text.empty()) {
        context_.WriteClipboardText(text);
        context_.WritePrimarySelectionText(text);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CopyLastTerminalCommand: {
      const std::optional<std::string> text = context_.LastTerminalCommandText();
      if (text.has_value()) {
        context_.WriteClipboardText(*text);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CopySelectionWithContext: {
      const std::optional<std::string> text = context_.SelectionTextWithContext();
      if (text.has_value()) {
        context_.WriteClipboardText(*text);
        context_.WritePrimarySelectionText(*text);
      }
      return DispatchResult::Handled;
    }
    case ActionId::CutSelection: {
      context_.CutSelection();
      return DispatchResult::Handled;
    }
    case ActionId::PasteClipboard: {
      context_.PasteClipboard();
      return DispatchResult::Handled;
    }
    case ActionId::InsertText: {
      // `type <text>`: insert literal text at the caret. Args are already
      // shell-tokenized (quotes/escapes handled by ParseCommandLine), so join
      // them with single spaces — `type foo bar` and `type "foo bar"` both
      // insert `foo bar`.
      std::string text;
      for (const std::string& arg : args) {
        if (!text.empty()) text.push_back(' ');
        text += arg;
      }
      if (!text.empty()) {
        context_.InsertText(std::move(text));
      }
      return DispatchResult::Handled;
    }
    case ActionId::InlineCompletion: {
      std::string error_message;
      if (!context_.RequestInlineCompletion(&error_message)) {
        return reject(error_message.empty() ? "Inline completion unavailable" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::TestsDiscover: {
      std::string error_message;
      if (!context_.DiscoverTestsForActiveBuffer(&error_message)) {
        return reject(error_message.empty() ? "Test discovery failed" : error_message);
      }
      return DispatchResult::Handled;
    }
    case ActionId::JumpToMatchingBracket: {
      auto* viewport = context_.ActiveNavigableViewport();
      if (viewport == nullptr) return DispatchResult::Handled;
      auto match = editor::FindBracketMatch(*viewport, viewport->cursor_line(),
                                            viewport->cursor_column());
      if (!match) return DispatchResult::Handled;
      const bool at_open = (viewport->cursor_line() == match->open_line &&
                            (viewport->cursor_column() == match->open_column ||
                             viewport->cursor_column() == match->open_column + 1));
      if (at_open) {
        viewport->MoveCursorTo(match->close_line, match->close_column, false);
      } else {
        viewport->MoveCursorTo(match->open_line, match->open_column, false);
      }
      context_.NotifyEditorCaretMoved();
      return DispatchResult::Handled;
    }
    case ActionId::ToggleLineComment:
    case ActionId::ToggleBlockComment:
    case ActionId::MoveLineUp:
    case ActionId::MoveLineDown:
    case ActionId::DuplicateLine:
    case ActionId::DeleteLine:
    case ActionId::IndentLines:
    case ActionId::OutdentLines:
    case ActionId::SortLinesAscending:
    case ActionId::SortLinesDescending: {
      auto* viewport = context_.ActiveEditableViewport();
      if (viewport == nullptr) return DispatchResult::Handled;
      const bool is_comment_action =
          id == ActionId::ToggleLineComment || id == ActionId::ToggleBlockComment;
      const bool is_line_op_action =
          id == ActionId::MoveLineUp || id == ActionId::MoveLineDown ||
          id == ActionId::DuplicateLine || id == ActionId::DeleteLine ||
          id == ActionId::IndentLines || id == ActionId::OutdentLines;
      const bool is_sort_action =
          id == ActionId::SortLinesAscending || id == ActionId::SortLinesDescending;

      if (is_comment_action && !SettingEnabled(context_, "editor.shaping.toggle_comment.enabled", true)) {
        return DispatchResult::Handled;
      }
      if (is_line_op_action && !SettingEnabled(context_, "editor.shaping.line_ops.enabled", true)) {
        return DispatchResult::Handled;
      }
      if (is_sort_action && !SettingEnabled(context_, "editor.shaping.sort_lines.enabled", true)) {
        return DispatchResult::Handled;
      }
      bool changed = false;
      switch (id) {
        case ActionId::ToggleLineComment: {
          // Comment markers are language-dependent: read them from the buffer's
          // resolved language contract (populated by ApplyEditorPreferences on
          // open/language change), falling back to C-style when a language
          // provides none.
          const editor::LanguageContractView& lc = viewport->language_contract_view();
          const std::string_view line_marker =
              lc.line_comment.empty() ? std::string_view("//")
                                      : std::string_view(lc.line_comment);
          changed = editor::ToggleLineComment(*viewport, line_marker);
          break;
        }
        case ActionId::ToggleBlockComment: {
          const editor::LanguageContractView& lc = viewport->language_contract_view();
          const bool has_block =
              !lc.block_comment_open.empty() && !lc.block_comment_close.empty();
          const std::string_view open =
              has_block ? std::string_view(lc.block_comment_open) : std::string_view("/*");
          const std::string_view close =
              has_block ? std::string_view(lc.block_comment_close) : std::string_view("*/");
          changed = editor::ToggleBlockComment(*viewport, open, close);
          break;
        }
        case ActionId::MoveLineUp:
          changed = editor::MoveLineUp(*viewport);
          break;
        case ActionId::MoveLineDown:
          changed = editor::MoveLineDown(*viewport);
          break;
        case ActionId::DuplicateLine:
          changed = editor::DuplicateSelection(*viewport);
          break;
        case ActionId::DeleteLine:
          changed = editor::DeleteLine(*viewport);
          break;
        case ActionId::IndentLines:
          changed = editor::IndentSelection(*viewport);
          break;
        case ActionId::OutdentLines:
          changed = editor::OutdentSelection(*viewport);
          break;
        case ActionId::SortLinesAscending:
          changed = editor::SortLines(*viewport, /*ascending=*/true);
          break;
        case ActionId::SortLinesDescending:
          changed = editor::SortLines(*viewport, /*ascending=*/false);
          break;
        default: break;
      }
      if (changed) {
        context_.NotifyEditorViewportChanged(/*last_change=*/true);
      }
      return DispatchResult::Handled;
    }
    case ActionId::AddCursorAtNextMatch:
    case ActionId::AddCursorAtAllMatches: {
      if (!SettingEnabled(context_, "editor.multicursor.add_at_match.enabled", true)) {
        return DispatchResult::Handled;
      }
      auto* viewport = context_.ActiveEditableViewport();
      if (viewport == nullptr) return DispatchResult::Handled;
      // If no selection, expand to word under caret first.
      if (!viewport->has_selection()) {
        viewport->SelectWordAtCursor();
      }
      auto sel = viewport->selection_range();
      if (!sel || sel->start.line != sel->end.line || sel->start.column == sel->end.column) {
        return DispatchResult::Handled;
      }
      // Scan through the piece tree's zero-copy LineView rather than snapshotting
      // the whole document into a vector<std::string> on every press.
      const editor::TextBuffer& lines = viewport->lines();
      if (sel->start.line >= lines.LineCount()) return DispatchResult::Handled;
      const std::string_view line = lines.LineView(sel->start.line);
      std::size_t a = std::min(sel->start.column, sel->end.column);
      std::size_t b = std::max(sel->start.column, sel->end.column);
      if (b > line.size()) return DispatchResult::Handled;
      const std::string needle(line.substr(a, b - a));
      if (needle.empty()) return DispatchResult::Handled;
      const std::string_view needle_view = needle;
      const bool case_sensitive = SettingEnabled(context_, "editor.search.case_sensitive", false);
      if (id == ActionId::AddCursorAtNextMatch) {
        if (const auto next = FindNextLiteralMatchAfterSeedWrapOnce(
                lines, sel->start.line, a, b, needle_view, case_sensitive);
            next.has_value()) {
          // Add the match as a RANGED secondary caret (anchor at match start,
          // cursor at match end) so multi-caret typing replaces the occurrence
          // and copy aggregates it -- VS Code parity. Bare positions through
          // SetSecondaryCarets would drop the selection anchor. This appends to
          // and preserves any secondary carets from prior presses.
          viewport->AddSecondaryCaretWithRange(editor::SelectionRange{
              editor::TextPosition{next->line, next->column},
              editor::TextPosition{next->line, next->column + needle_view.size()},
          });
        }
      } else {
        // Add a ranged cursor at every match in the file, each keeping its
        // selection so a following keystroke replaces all occurrences at once.
        std::vector<editor::SelectionRange> ranges;
        for (std::size_t li = 0; li < lines.LineCount(); ++li) {
          const std::string_view current = lines.LineView(li);
          std::size_t from = 0;
          while (true) {
            const auto pos =
                FindLiteralNeedleInLine(current, from, needle_view, case_sensitive);
            if (!pos.has_value()) {
              break;
            }
            // Skip the seed selection; the primary caret already covers it.
            if (li == sel->start.line && *pos == a) {
              from = *pos + needle_view.size();
              continue;
            }
            ranges.push_back(editor::SelectionRange{
                editor::TextPosition{li, *pos},
                editor::TextPosition{li, *pos + needle_view.size()},
            });
            from = *pos + needle_view.size();
            if (from >= current.size()) break;
          }
        }
        viewport->SetSecondaryCaretsWithRanges(std::move(ranges));
      }
      context_.NotifyEditorCaretMoved();
      return DispatchResult::Handled;
    }
    case ActionId::Fold:
    case ActionId::Unfold:
    case ActionId::ToggleFoldAtCursor:
    case ActionId::FoldAll:
    case ActionId::UnfoldAll: {
      auto* model = context_.EnsureActiveFoldingModelFresh();
      if (model == nullptr) return DispatchResult::Handled;
      bool changed = false;
      if (id == ActionId::FoldAll) {
        if (!model->ranges().empty()) {
          model->CollapseAll();
          changed = true;
        }
      } else if (id == ActionId::UnfoldAll) {
        if (!model->ranges().empty()) {
          model->ExpandAll();
          changed = true;
        }
      } else {
        auto* viewport = context_.ActiveNavigableViewport();
        if (viewport == nullptr) return DispatchResult::Handled;
        const std::size_t caret_line = viewport->cursor_line();
        // Find the innermost fold whose opener_line <= caret_line <= closer_line.
        std::optional<std::size_t> target_opener;
        std::size_t best_span = static_cast<std::size_t>(-1);
        for (const auto& range : model->ranges()) {
          if (caret_line < range.opener_line || caret_line > range.closer_line) continue;
          const std::size_t span = range.closer_line - range.opener_line;
          if (span < best_span) {
            best_span = span;
            target_opener = range.opener_line;
          }
        }
        if (!target_opener) return DispatchResult::Handled;
        if (id == ActionId::Fold) {
          changed = model->Collapse(*target_opener);
        } else if (id == ActionId::Unfold) {
          changed = model->Expand(*target_opener);
        } else {
          changed = model->ToggleFold(*target_opener);
        }
      }
      if (changed) {
        context_.NotifyEditorViewportChanged(/*last_change=*/false);
      }
      return DispatchResult::Handled;
    }
    case ActionId::ToggleEditorFolding:
    case ActionId::ToggleEditorStickyScroll:
    case ActionId::ToggleEditorIndentGuides:
    case ActionId::ToggleEditorRenderWhitespace:
    case ActionId::ToggleEditorBracketMatchHighlight:
    case ActionId::ToggleEditorAutoClosePairs:
    case ActionId::ToggleEditorSurround:
    case ActionId::ToggleEditorSmartIndent:
    case ActionId::ToggleEditorToggleComment:
    case ActionId::ToggleEditorLineOps:
    case ActionId::ToggleEditorSortLines:
    case ActionId::ToggleEditorAddCursorAtMatch:
    case ActionId::ToggleEditorOccurrencesHighlight:
    case ActionId::ToggleEditorSearchCaseSensitive:
    case ActionId::ToggleEditorSnippets:
    case ActionId::ToggleEditorSaveTrim:
    case ActionId::ToggleEditorSaveEnsureNewline:
    case ActionId::ToggleEditorAutoDetectIndent: {
      // Toggle commands flip the corresponding setting key. Settings are read
      // by feature consumers via `WorkspaceContext::user_settings`; this
      // command is the canonical way to flip them from a keybinding or menu
      // entry.
      context_.ToggleEditorEssentialsCapability(id);
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
