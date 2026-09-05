#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"
#include "workspace/actions/WorkspaceActionServices.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "editor/BracketScanner.h"
#include "editor/ColumnSelection.h"
#include "editor/ShapingActions.h"
#include "editor/TextViewport.h"
#include "util/JsonFormat.h"
#include "workspace/SettingFlags.h"
#include "workspace/actions/WorkspaceActionRequests.h"
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

  // Settings-backed editor-essentials toggles flip their setting key and nothing
  // else, and EditorEssentialsCapabilitySettingKey already knows which actions
  // those are — it is what the executor and the menu's checked state both read.
  // Listing the eighteen of them again as case labels only created a second
  // place to forget a new one.
  if (EditorEssentialsCapabilitySettingKey(id) != nullptr) {
    context_.ToggleEditorEssentialsCapability(id);
    return DispatchResult::Handled;
  }

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
    case ActionId::FormatJson: {
      // Optional path argument: open/focus that file first (resolved against the
      // project root like `open`), then format its buffer. With no argument the
      // command targets the active buffer. Feedback surfaces via toast per the
      // command's contract; disk is never touched (the edit stays in memory).
      if (!args.empty()) {
        if (!context_.HasProjectRoot()) {
          context_.Notify(NotificationService::Tone::Error, "No active project");
          return DispatchResult::Handled;
        }
        const std::optional<OpenPathRequest> request =
            BuildOpenPathRequest(args, context_.ProjectRoot());
        if (!request.has_value()) {
          context_.Notify(NotificationService::Tone::Error, "format-json requires a valid path");
          return DispatchResult::Handled;
        }
        std::string open_error;
        if (!context_.OpenPath(request->path, &open_error)) {
          context_.Notify(NotificationService::Tone::Error,
                          open_error.empty() ? "Could not open file" : open_error);
          return DispatchResult::Handled;
        }
      }
      auto* viewport = context_.ActiveEditableViewport();
      if (viewport == nullptr) {
        context_.Notify(NotificationService::Tone::Warning, "No active buffer to format");
        return DispatchResult::Handled;
      }
      // The piece tree already holds the document '\n'-joined, so take it in one
      // walk rather than two tree descents per line (which also materializes
      // every piece-spanning line into the per-line cache on the way).
      std::string source;
      viewport->lines().AppendWholeText(source);
      // Indentation follows the buffer's own editor settings (VSCode-style).
      const std::string indent_unit = viewport->soft_tabs()
                                          ? std::string(viewport->indent_width(), ' ')
                                          : std::string("\t");
      const util::JsonFormatResult formatted = util::FormatJson(source, indent_unit);
      if (!formatted.ok) {
        // Byte offset -> 1-based line:col for a helpful toast.
        std::size_t line = 1;
        std::size_t col = 1;
        for (std::size_t i = 0; i < formatted.error_offset && i < source.size(); ++i) {
          if (source[i] == '\n') {
            ++line;
            col = 1;
          } else {
            ++col;
          }
        }
        context_.Notify(NotificationService::Tone::Error,
                        "Invalid JSON (" + std::to_string(line) + ":" + std::to_string(col) +
                            "): " + formatted.error);
        return DispatchResult::Handled;
      }
      if (formatted.text == source) {
        // Already formatted: no edit, no undo churn (VSCode is silent here).
        return DispatchResult::Handled;
      }
      // Replace the whole document as one undo step (the same whole-document
      // replace path editor::SortLines uses). The formatter emits '\n'-joined
      // lines with no trailing newline.
      std::vector<std::string> new_lines;
      {
        std::size_t start = 0;
        const std::string& text = formatted.text;
        for (std::size_t i = 0; i <= text.size(); ++i) {
          if (i == text.size() || text[i] == '\n') {
            new_lines.emplace_back(text.substr(start, i - start));
            start = i + 1;
          }
        }
      }
      if (viewport->ReplaceLines(0, viewport->line_count(), std::move(new_lines),
                                 /*record_undo=*/true)) {
        context_.NotifyEditorViewportChanged(/*last_change=*/true);
      }
      return DispatchResult::Handled;
    }
    case ActionId::RenameSymbol: {
      // `rename-symbol <new-name>` renames outright (a headless driver cannot
      // answer a prompt); bare, it opens the prompt prefilled with the symbol.
      if (!args.empty()) {
        std::string error_message;
        if (!context_.RenameSymbol(JoinCommandArguments(args, 0), &error_message)) {
          return reject(error_message.empty() ? "Rename unavailable" : error_message);
        }
        return DispatchResult::Handled;
      }
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
    case ActionId::CallHierarchy: {
      // `call-hierarchy [incoming|outgoing]`, defaulting to incoming — "who calls
      // this?" is the question that gets asked, and it is what VS Code opens with.
      bool incoming = true;
      if (!args.empty()) {
        if (args[0] == "outgoing") {
          incoming = false;
        } else if (args[0] != "incoming") {
          return reject("Usage: call-hierarchy [incoming|outgoing]");
        }
      }
      std::string error_message;
      if (!context_.ShowCallHierarchy(incoming, &error_message)) {
        return reject(error_message.empty() ? "Call hierarchy unavailable" : error_message);
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

      // `goto` is an ABSOLUTE 1-based line per its public contract (README, control
      // protocol). Reject any non-positive line with feedback rather than silently
      // succeeding: a negative value previously fell through to the "from end" mode in
      // ExecuteLineNavigation (goto -1 -> last line), which contradicts the documented
      // absolute contract and made typos navigate to EOF. `jump` keeps signed relative
      // deltas. (TD-2026-07-16-68.)
      if (id == ActionId::Goto && request->requested_line <= 0) {
        return reject("Go to Line expects a positive 1-based line number");
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
        viewport->JumpCursorTo(match->close_line, match->close_column, false);
      } else {
        viewport->JumpCursorTo(match->open_line, match->open_column, false);
      }
      context_.NotifyEditorCaretMoved();
      return DispatchResult::Handled;
    }
    case ActionId::ColumnSelectUp:
    case ActionId::ColumnSelectDown:
    case ActionId::ColumnSelectLeft:
    case ActionId::ColumnSelectRight: {
      auto* viewport = context_.ActiveEditableViewport();
      if (viewport == nullptr) {
        return DispatchResult::Handled;
      }
      const editor::ColumnSelectDirection direction =
          id == ActionId::ColumnSelectUp     ? editor::ColumnSelectDirection::Up
          : id == ActionId::ColumnSelectDown ? editor::ColumnSelectDirection::Down
          : id == ActionId::ColumnSelectLeft ? editor::ColumnSelectDirection::Left
                                             : editor::ColumnSelectDirection::Right;
      const editor::ColumnSelectionState before = viewport->column_selection();
      // The gesture works in visual columns (a step is one cell, and the box stays
      // straight across tabs and multi-byte text), so the caret enters as the
      // visual column it occupies.
      const editor::TextPosition caret{viewport->cursor_line(), viewport->cursor_visual_column()};
      const std::size_t lo =
          before.active ? std::min(before.anchor.line, before.cursor.line) : caret.line;
      const std::size_t hi =
          before.active ? std::max(before.anchor.line, before.cursor.line) : caret.line;
      // The virtual column may only grow to the widest line the box currently
      // covers; unbounded growth would let Right run forever over short lines.
      const editor::ColumnSelectionState after = editor::StepColumnSelection(
          before, direction, caret, viewport->line_count(),
          viewport->MaxVisualWidthInSpan(lo, hi));
      viewport->SetColumnSelection(after);
      viewport->SetBoxSelectionVisual(after.anchor.line, after.anchor.column, after.cursor.line,
                                      after.cursor.column);
      context_.NotifyEditorCaretMoved();
      return DispatchResult::Handled;
    }
    case ActionId::ToggleLineComment:
    case ActionId::ToggleBlockComment:
    case ActionId::MoveLineUp:
    case ActionId::MoveLineDown:
    case ActionId::DuplicateLine:
    case ActionId::CopyLineUp:
    case ActionId::InsertLineBelow:
    case ActionId::InsertLineAbove:
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
          id == ActionId::DuplicateLine || id == ActionId::CopyLineUp ||
          id == ActionId::InsertLineBelow || id == ActionId::InsertLineAbove ||
          id == ActionId::DeleteLine ||
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
          changed = editor::CopyLines(*viewport, /*downward=*/true);
          break;
        case ActionId::CopyLineUp:
          changed = editor::CopyLines(*viewport, /*downward=*/false);
          break;
        case ActionId::InsertLineBelow:
          changed = editor::InsertLineBelow(*viewport);
          break;
        case ActionId::InsertLineAbove:
          changed = editor::InsertLineAbove(*viewport);
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
      // No selection: the word under the caret becomes the needle. For the
      // next-match chord that IS the first press (VS Code's Ctrl+D selects the
      // word and stops; the next press adds the next occurrence); select-all
      // goes on to take every occurrence at once.
      if (!viewport->has_selection()) {
        viewport->SelectWordAtCursor();
        if (id == ActionId::AddCursorAtNextMatch) {
          context_.NotifyEditorCaretMoved();
          return DispatchResult::Handled;
        }
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
        // Each press adds the next occurrence that is not a caret yet, walking
        // forward from the one the previous press added (VS Code's Ctrl+D). The
        // search is seeded from the primary selection and wraps once; it used to
        // stop there, so the third press re-found the second press's match and
        // the dedupe swallowed it -- two carets was the most the chord could make.
        const std::span<const editor::TextViewportUndoHistory::SecondaryCaret> taken =
            viewport->secondary_caret_range_view();
        std::size_t seed_line = sel->start.line;
        std::size_t seed_start = a;
        std::size_t seed_end = b;
        // The presses so far took a prefix of the occurrences in cyclic document
        // order from the primary, so the one added last is the farthest along
        // that order: the last caret before the primary if the walk has wrapped,
        // else the last caret after it. Seeding there makes a press one search;
        // the hop loop below only has to skip carets placed by other means.
        const editor::TextPosition primary_start{sel->start.line, a};
        const auto precedes = [](const editor::TextPosition& lhs, const editor::TextPosition& rhs) {
          return lhs.line < rhs.line || (lhs.line == rhs.line && lhs.column < rhs.column);
        };
        const editor::TextViewportUndoHistory::SecondaryCaret* last_added = nullptr;
        bool wrapped = false;
        for (const auto& caret : taken) {
          if (!caret.selection_anchor.has_value()) {
            continue;
          }
          const bool before_primary = precedes(caret.position, primary_start);
          if (last_added == nullptr || (before_primary && !wrapped) ||
              (before_primary == wrapped && precedes(last_added->position, caret.position))) {
            last_added = &caret;
            wrapped = before_primary;
          }
        }
        if (last_added != nullptr && last_added->selection_anchor->line == last_added->position.line) {
          seed_line = last_added->position.line;
          seed_start = std::min(last_added->selection_anchor->column, last_added->position.column);
          seed_end = std::max(last_added->selection_anchor->column, last_added->position.column);
        }
        std::optional<editor::SelectionRange> fresh;
        for (std::size_t hops = 0; hops <= taken.size(); ++hops) {
          const auto next = FindNextLiteralMatchAfterSeedWrapOnce(
              lines, seed_line, seed_start, seed_end, needle_view, case_sensitive);
          if (!next.has_value() || (next->line == sel->start.line && next->column == a)) {
            break;  // wrapped back to the primary: every occurrence is a caret already
          }
          const editor::SelectionRange range{
              editor::TextPosition{next->line, next->column},
              editor::TextPosition{next->line, next->column + needle_view.size()},
          };
          const bool already_a_caret = std::any_of(
              taken.begin(), taken.end(),
              [&](const editor::TextViewportUndoHistory::SecondaryCaret& caret) {
                return caret.position == range.end && caret.selection_anchor == range.start;
              });
          if (!already_a_caret) {
            fresh = range;
            break;
          }
          seed_line = range.start.line;
          seed_start = range.start.column;
          seed_end = range.end.column;
        }
        if (fresh.has_value()) {
          // Add the match as a RANGED secondary caret (anchor at match start,
          // cursor at match end) so multi-caret typing replaces the occurrence
          // and copy aggregates it -- VS Code parity. Bare positions through
          // SetSecondaryCarets would drop the selection anchor. This appends to
          // and preserves any secondary carets from prior presses.
          viewport->AddSecondaryCaretWithRange(*fresh);
        }
      } else {
        // Add a ranged cursor at every match in the file, each keeping its
        // selection so a following keystroke replaces all occurrences at once.
        // CollectAddCursorMatchRanges folds each line once (not once per match)
        // and caps the installed carets so a dense single-line match set stays
        // bounded (TD-2026-07-17A-031).
        AddCursorMatchScan scan = CollectAddCursorMatchRanges(
            lines, sel->start.line, a, needle_view, case_sensitive);
        const std::size_t match_count = scan.ranges.size();
        const bool truncated = scan.truncated;
        viewport->SetSecondaryCaretsWithRanges(scan.ranges);
        if (truncated) {
          context_.Notify(
              NotificationService::Tone::Warning,
              "Added cursors at the first " + std::to_string(match_count) +
                  " matches (more remain)");
        }
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
      auto* viewport = context_.ActiveNavigableViewport();
      bool changed = false;
      if (id == ActionId::FoldAll) {
        // The per-frame refresh only resolves the viewport's window, so "all"
        // has to widen it to the whole document first.
        if (viewport == nullptr) return DispatchResult::Handled;
        model->ResolveAllFolds(viewport->lines(), viewport);
        changed = model->CollapseAllResolved();
      } else if (id == ActionId::UnfoldAll) {
        if (model->has_any_collapsed_fold()) {
          model->ExpandAll();
          changed = true;
        }
      } else {
        if (viewport == nullptr) return DispatchResult::Handled;
        // The innermost fold whose opener_line <= caret_line <= closer_line.
        const std::optional<editor::FoldRange> target =
            model->InnermostFoldContaining(viewport->cursor_line());
        if (!target) return DispatchResult::Handled;
        if (id == ActionId::Fold) {
          changed = model->Collapse(target->opener_line);
        } else if (id == ActionId::Unfold) {
          changed = model->Expand(target->opener_line);
        } else {
          changed = model->ToggleFold(target->opener_line);
        }
      }
      if (changed) {
        context_.NotifyEditorViewportChanged(/*last_change=*/false);
      }
      return DispatchResult::Handled;
    }
    default:
      return DispatchResult::Unhandled;
  }
}

}  // namespace microide::workspace
