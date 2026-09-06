#include "editor/TextViewport.h"
#include "editor/TextViewportInternal.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextLayout.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"

namespace microide::editor {


void TextViewport::InsertCharacter(char character) {
  if (has_multiple_carets()) {
    if (TryMultiCaretPairInsert(character)) {
      return;
    }
    const std::string text(1, character);
    (void)ApplyMultiCaretInsert(text, true);
    return;
  }
  if (TrySurroundInsert(character)) {
    return;
  }
  if (TrySkipOverClose(character)) {
    return;
  }
  (void)MaybeDedentOnClose(character);
  if (TryAutoCloseInsert(character)) {
    return;
  }
  const SelectionRange range = selection_range().value_or(
      SelectionRange{TextPosition{cursor_line_, cursor_column_},
                     TextPosition{cursor_line_, cursor_column_}});
  const std::string text(1, character);
  const CoalesceHint hint{
      .kind = CoalesceKind::Insert,
      .changed_is_space = util::IsAsciiSpace(static_cast<unsigned char>(character)) != 0,
  };
  (void)ApplyRangeEdit(range, text, true, hint);
}

void TextViewport::InsertText(std::string_view text, bool record_undo) {
  if (text.empty()) {
    return;
  }
  if (text.size() == 1 && text.front() != '\n' && record_undo) {
    InsertCharacter(text.front());
    return;
  }
  if (has_multiple_carets()) {
    (void)ApplyMultiCaretInsert(text, record_undo);
    return;
  }

  const SelectionRange range = selection_range().value_or(
      SelectionRange{TextPosition{cursor_line_, cursor_column_},
                     TextPosition{cursor_line_, cursor_column_}});
  (void)ApplyRangeEdit(range, text, record_undo);
}

void TextViewport::InsertNewline() {
  if (has_multiple_carets()) {
    (void)ApplyMultiCaretInsert("\n", true);
    return;
  }
  if (TryInsertNewlineSplitBraces()) {
    return;
  }
  const SelectionRange range = selection_range().value_or(
      SelectionRange{TextPosition{cursor_line_, cursor_column_},
                     TextPosition{cursor_line_, cursor_column_}});
  // Auto-indent from the insertion point (the surviving prefix line), which is
  // range.start after the selection is deleted — not cursor_line_/column_, which
  // for a top-to-bottom selection sits on the line being removed at range.end.
  // Matches the multi-caret path (TextViewportMultiCaret uses removed.start).
  const std::string newline_text =
      "\n" + AutoIndentForNewline(range.start.line, range.start.column);
  (void)ApplyRangeEdit(range, newline_text, true);
}

void TextViewport::InsertTab() {
  if (!soft_tabs_) {
    InsertCharacter('\t');
    return;
  }

  // Multi-caret: each caret aligns to its OWN next tab stop rather than reusing the
  // primary caret's column for identical padding at every (ragged-column) caret.
  if (has_multiple_carets()) {
    (void)ApplyMultiCaretSoftTab(true);
    return;
  }

  const std::size_t safe_indent_width = std::max<std::size_t>(1, indent_width_);
  const std::size_t visual_column = cursor_visual_column();
  const std::size_t remainder = visual_column % safe_indent_width;
  const std::size_t spaces =
      remainder == 0 ? safe_indent_width : safe_indent_width - remainder;
  InsertText(std::string(std::max<std::size_t>(1, spaces), ' '));
}

std::optional<std::size_t> TextViewport::IndentStopBackspaceStart(std::size_t line,
                                                                   std::size_t column) const {
  if (!soft_tabs_ || column == 0 || line >= document_->lines.size()) {
    return std::nullopt;
  }
  // The prefix has to be read to know it is all whitespace. An indent deeper
  // than this is not a workflow, and reading the caret's whole prefix on every
  // Backspace is exactly the per-keystroke O(line) cost a file with no line
  // breaks in it cannot afford (TD-2026-08-05-133); past the cap the ordinary
  // one-code-point delete applies.
  constexpr std::size_t kMaxIndentScan = 4096;
  if (column > kMaxIndentScan) {
    return std::nullopt;
  }
  std::string scratch;
  const std::string_view prefix = document_->lines.LineWindow(line, 0, column, scratch);
  if (prefix.size() != column ||
      prefix.find_first_not_of(" \t") != std::string_view::npos) {
    return std::nullopt;
  }
  const std::size_t width = std::max<std::size_t>(1, indent_width_);
  const std::size_t visual = TextLayout::VisualColumnForTextColumn(prefix, column, tab_size_);
  if (visual == 0) {
    return std::nullopt;
  }
  const std::size_t target_visual = ((visual - 1) / width) * width;
  const std::size_t target = TextLayout::TextColumnForVisualColumn(prefix, target_visual, tab_size_);
  // A stop that falls inside a hard tab of the prefix resolves to the tab's
  // own boundary; when that is not before the caret, delete the one unit.
  if (target >= column) {
    return std::nullopt;
  }
  return target;
}

void TextViewport::Backspace() {
  util::PerformanceTrace::Scope perf_scope("TextViewport::Backspace");
  if (document_->lines.empty()) {
    return;
  }
  if (has_multiple_carets()) {
    (void)ApplyMultiCaretBackspace(true);
    return;
  }

  if (const auto selected = selection_range(); selected.has_value()) {
    (void)ApplyRangeEdit(*selected, "", true);
    return;
  }

  if (cursor_column_ > 0) {
    // LineView, not operator[]: the latter is TextBuffer's compatibility accessor
    // and materializes an owned copy of the line into a per-revision cache that
    // the very next edit clears. On the single hottest path in the editor that is
    // a full line copy per keystroke -- invisible on a 40-character line, the
    // whole document on a file with no newlines in it.
    // Everything this needs is the code point immediately before the caret. It
    // used to read the whole line to find it, which on a file with no line breaks
    // in it is megabytes per backspace (TD-2026-08-05-133).
    const CaretNeighborhood at = ReadCaretNeighborhood(cursor_line_, cursor_column_);
    // In leading whitespace with soft tabs the deletion runs back to the
    // previous indent stop (VS Code's useTabStops); the byte before the caret
    // is a space then, so the cheap neighbourhood test gates the prefix read.
    const bool prev_is_space = at.has_prev && (at.prev_char == ' ' || at.prev_char == '\t');
    const std::size_t erase_start =
        prev_is_space
            ? IndentStopBackspaceStart(cursor_line_, at.clamped_column).value_or(at.prev_column)
            : (at.has_prev ? at.prev_column : 0);
    // Backspace inside `(|)` removes the pair auto-close put there, as VS Code's
    // autoClosingDelete does; the closer is one byte by the opener rule.
    const std::size_t erase_end = CaretSitsInsideAutoClosedPair(cursor_line_, at.clamped_column)
                                      ? at.clamped_column + 1
                                      : cursor_column_;
    const CoalesceHint hint{
        .kind = CoalesceKind::DeleteBackward,
        .changed_is_space =
            at.has_prev && util::IsAsciiSpace(static_cast<unsigned char>(at.prev_char)) != 0,
    };
    (void)ApplyRangeEdit(
        SelectionRange{
            .start = TextPosition{cursor_line_, erase_start},
            .end = TextPosition{cursor_line_, erase_end},
        },
        "", true, hint);
    return;
  }

  if (cursor_line_ == 0) {
    return;
  }

  (void)ApplyRangeEdit(
      SelectionRange{
          .start = TextPosition{cursor_line_ - 1, document_->lines.LineLength(cursor_line_ - 1)},
          .end = TextPosition{cursor_line_, 0},
      },
      "", true);
}

void TextViewport::DeleteWord(int direction) {
  util::PerformanceTrace::Scope perf_scope("TextViewport::DeleteWord");
  if (document_->lines.empty() || direction == 0) {
    return;
  }
  if (has_multiple_carets()) {
    (void)ApplyMultiCaretEdit(direction < 0 ? MultiCaretEditKind::DeleteWordBackward
                                            : MultiCaretEditKind::DeleteWordForward,
                              "", true);
    return;
  }

  // A word delete over a selection is a selection delete, exactly as in VS Code:
  // the modifier does not widen what is already explicitly selected.
  if (const auto selected = selection_range(); selected.has_value()) {
    (void)ApplyRangeEdit(*selected, "", true);
    return;
  }

  const TextPosition caret{cursor_line_, cursor_column_};
  const TextPosition target = WordTargetForCaret(caret, direction, /*for_deletion=*/true);
  if (target.line == caret.line && target.column == caret.column) {
    return;
  }
  // A word delete is never coalesced into the neighbouring single-character undo
  // entry: Ctrl+Backspace is one user-visible action and one Undo must put the
  // whole word back.
  (void)ApplyRangeEdit(direction < 0 ? SelectionRange{target, caret} : SelectionRange{caret, target},
                       "", true);
}

void TextViewport::DeleteForward() {
  util::PerformanceTrace::Scope perf_scope("TextViewport::DeleteForward");
  if (document_->lines.empty()) {
    return;
  }
  if (has_multiple_carets()) {
    (void)ApplyMultiCaretDeleteForward(true);
    return;
  }

  if (const auto selected = selection_range(); selected.has_value()) {
    (void)ApplyRangeEdit(*selected, "", true);
    return;
  }

  const std::string_view line = document_->lines.LineView(cursor_line_);  // see Backspace
  if (cursor_column_ < line.size()) {
    const std::size_t erase_end = TextLayout::NextTextColumn(line, cursor_column_);
    const CoalesceHint hint{
        .kind = CoalesceKind::DeleteForward,
        .changed_is_space = util::IsAsciiSpace(static_cast<unsigned char>(line[cursor_column_])) != 0,
    };
    (void)ApplyRangeEdit(
        SelectionRange{
            .start = TextPosition{cursor_line_, cursor_column_},
            .end = TextPosition{cursor_line_, erase_end},
        },
        "", true, hint);
    return;
  }

  if (cursor_line_ + 1 >= document_->lines.size()) {
    return;
  }

  (void)ApplyRangeEdit(
      SelectionRange{
          .start = TextPosition{cursor_line_, cursor_column_},
          .end = TextPosition{cursor_line_ + 1, 0},
      },
      "", true);
}

bool TextViewport::ApplyHistoryStep(bool redo) {
  util::PerformanceTrace::Scope perf_scope(redo ? "TextViewport::Redo" : "TextViewport::Undo");
  if (document_->undo_history.IsGroupActive()) {
    FlushActiveUndoGroup();
  }
  if (redo ? !document_->undo_history.CanRedo() : !document_->undo_history.CanUndo()) {
    return false;
  }

  HistoryEntry entry = redo ? document_->undo_history.PopRedo() : document_->undo_history.PopUndo();
  // Stamp the view state for the side we are leaving, so stepping back lands the
  // caret and scroll where they were.
  (redo ? entry.before_state : entry.after_state) = CaptureViewState();
  {
    util::PerformanceTrace::Scope scope(redo ? "TextViewport::Redo::ApplyHistoryEntry"
                                             : "TextViewport::Undo::ApplyHistoryEntry");
    ApplyHistoryEntry(entry, redo);
  }
  {
    util::PerformanceTrace::Scope scope(redo ? "TextViewport::Redo::BuildAppliedEdit"
                                             : "TextViewport::Undo::BuildAppliedEdit");
    SetLastAppliedEditFromEntry(entry, redo);
  }
  if (redo) {
    document_->undo_history.PushUndo(std::move(entry));
  } else {
    document_->undo_history.PushRedo(std::move(entry));
  }
  return true;
}

bool TextViewport::TopUndoEntryIsColumnScopedForTesting() const {
  const HistoryEntry* top = document_->undo_history.TopUndoEntry();
  return top != nullptr && top->is_inline;
}

bool TextViewport::Undo() { return ApplyHistoryStep(/*redo=*/false); }

bool TextViewport::Redo() { return ApplyHistoryStep(/*redo=*/true); }

bool TextViewport::ReplaceRange(const SelectionRange& range,
                                std::string_view replacement,
                                bool record_undo) {
  return ApplyRangeEdit(range, replacement, record_undo);
}

bool TextViewport::ReplaceLines(std::size_t start_line,
                                std::size_t end_line,
                                std::vector<std::string> replacement,
                                bool record_undo) {
  // Boundary overload: a caller that already owns a vector pays one pass to pack
  // it. Callers on the hot shaping paths build the blob directly (see
  // ReplaceLines(LineBlob) below) and skip that.
  return ApplyLineEdit(start_line, end_line, LineBlob(replacement), record_undo);
}

bool TextViewport::ReplaceLines(std::size_t start_line,
                                std::size_t end_line,
                                LineBlob replacement,
                                bool record_undo) {
  return ApplyLineEdit(start_line, end_line, std::move(replacement), record_undo);
}

std::size_t TextViewport::ReplaceAll(std::string_view needle, std::string_view replacement) {
  if (needle.empty() || document_->lines.empty()) {
    return 0;
  }

  const ViewState before_state = CaptureViewState();
  // Case fold (not just ASCII-lower) so replace matches non-ASCII case variants
  // (É/é, Δ/δ, …). Every covered fold is UTF-8 length-preserving, so folded byte
  // offsets stay aligned with the original line — the offset arithmetic below
  // (which indexes current_line by folded-buffer offsets) remains correct.
  const std::string lowered_needle = util::Utf8CaseFold(needle);
  // A replacement carrying a line break turns one source line into several
  // physical lines. Splitting it (below) keeps the PieceTree line_count consistent
  // instead of stuffing an embedded '\n' inside a single logical line — which
  // would leave LineCount() disagreeing with the buffer and corrupt later edits.
  const bool replacement_has_break =
      replacement.find('\n') != std::string_view::npos ||
      replacement.find('\r') != std::string_view::npos;
  std::size_t replacements = 0;
  std::size_t first_changed_line = document_->lines.size();
  std::size_t last_changed_line = 0;
  LineBlob before_changed_lines;
  LineBlob after_changed_lines;

  // Build the final content of the changed span in one pass, then apply it with a
  // single ReplaceLineRange so InvalidateDerivedCaches / RefreshEncoding run once
  // and multi-line replacements splice in as real lines.
  std::string new_line;
  std::string lowered_line;  // reused across lines to avoid a per-line allocation
  for (std::size_t line_index = 0; line_index < document_->lines.size(); ++line_index) {
    // A view until the line is known to change: this copied EVERY line of the
    // document into a fresh string before looking for the needle, one allocation
    // per line for a replace that touches a handful of them.
    const std::string_view current_line = document_->lines.LineView(line_index);
    util::Utf8CaseFoldInto(current_line, lowered_line);
    std::size_t offset = lowered_line.find(lowered_needle);
    if (offset == std::string::npos) {
      continue;
    }

    new_line.clear();
    new_line.reserve(current_line.size());
    std::size_t copy_from = 0;
    while (offset != std::string::npos) {
      new_line.append(current_line, copy_from, offset - copy_from);
      new_line.append(replacement);
      copy_from = offset + needle.size();
      ++replacements;
      // Search the unmodified lowered_line from copy_from so match offsets stay
      // in source-string coordinates (used to index current_line above). Mutating
      // lowered_line and resuming from the mutated position would desync the two
      // coordinate systems whenever replacement.size() != needle.size(), silently
      // corrupting every match after the first.
      offset = lowered_line.find(lowered_needle, copy_from);
    }
    new_line.append(current_line, copy_from);

    if (first_changed_line == document_->lines.size()) {
      first_changed_line = line_index;
    } else {
      // Carry the unchanged gap lines between the previous changed line and this
      // one so the before/after spans stay contiguous for a single range edit.
      for (std::size_t gap = last_changed_line + 1; gap < line_index; ++gap) {
        const std::string_view gap_line = document_->lines.LineView(gap);
        before_changed_lines.push_back(gap_line);
        after_changed_lines.push_back(gap_line);
      }
    }
    before_changed_lines.push_back(current_line);
    if (replacement_has_break) {
      // Views: the blob copies the bytes in, so owning a string per piece first
      // was one allocation per produced line for nothing.
      for (std::string_view piece : util::SplitLineViews(new_line)) {
        after_changed_lines.push_back(piece);
      }
    } else {
      after_changed_lines.push_back(new_line);
    }
    last_changed_line = line_index;
  }

  if (replacements > 0) {
    CommitLineRangeEdit(first_changed_line, std::move(before_changed_lines),
                        std::move(after_changed_lines), before_state);
  }
  return replacements;
}

void TextViewport::CommitLineRangeEdit(std::size_t first_changed_line,
                                       LineBlob before_changed_lines,
                                       LineBlob after_changed_lines,
                                       const ViewState& before_state) {
  document_->lines.ReplaceLineRange(first_changed_line, before_changed_lines.size(),
                                    after_changed_lines);
  document_->dirty = true;
  document_->undo_history.ClearRedo();
  RefreshEncoding();
  InvalidateLayoutCaches();
  EnsureCursorVisible();
  const ViewState after_state = CaptureViewState();
  PushHistoryEntry(HistoryEntry{
      .start_line = first_changed_line,
      .before_lines = std::move(before_changed_lines),
      .after_lines = std::move(after_changed_lines),
      .before_state = before_state,
      .after_state = after_state,
  });
}

std::optional<std::size_t> TextViewport::ReplaceAllRanges(
    const std::vector<SelectionRange>& matches, std::string_view replacement) {
  if (matches.empty() || document_->lines.empty()) {
    return std::size_t{0};
  }

  // Validate the caller's ranges before touching the document: single-line,
  // non-empty, in range, strictly ascending, and non-overlapping. Any violation
  // means the ranges are stale/inconsistent with the current buffer, so we bail
  // (nullopt) and leave the document untouched for the caller to rescan.
  std::size_t prev_line = 0;
  std::size_t prev_end_column = 0;
  bool have_prev = false;
  for (const SelectionRange& match : matches) {
    if (match.start.line != match.end.line || match.start.line >= document_->lines.size()) {
      return std::nullopt;
    }
    const std::string_view line = document_->lines.LineView(match.start.line);
    if (match.start.column >= match.end.column || match.end.column > line.size()) {
      return std::nullopt;
    }
    if (have_prev && (match.start.line < prev_line ||
                      (match.start.line == prev_line && match.start.column < prev_end_column))) {
      return std::nullopt;
    }
    prev_line = match.start.line;
    prev_end_column = match.end.column;
    have_prev = true;
  }

  const ViewState before_state = CaptureViewState();
  // A replacement carrying a line break splits one source line into several
  // physical lines, mirroring ReplaceAll.
  const bool replacement_has_break =
      replacement.find('\n') != std::string_view::npos ||
      replacement.find('\r') != std::string_view::npos;

  const std::size_t first_changed_line = matches.front().start.line;
  const std::size_t last_changed_line = matches.back().start.line;
  LineBlob before_changed_lines;
  LineBlob after_changed_lines;
  std::string new_line;

  std::size_t match_index = 0;
  for (std::size_t line_index = first_changed_line; line_index <= last_changed_line; ++line_index) {
    const std::string_view current_line = document_->lines.LineView(line_index);
    if (match_index >= matches.size() || matches[match_index].start.line != line_index) {
      // Unchanged gap line between changed lines: carry it into both spans so the
      // before/after ranges stay contiguous for a single ReplaceLineRange.
      before_changed_lines.push_back(current_line);
      after_changed_lines.push_back(current_line);
      continue;
    }

    new_line.clear();
    new_line.reserve(current_line.size());
    std::size_t copy_from = 0;
    while (match_index < matches.size() && matches[match_index].start.line == line_index) {
      const SelectionRange& match = matches[match_index];
      new_line.append(current_line, copy_from, match.start.column - copy_from);
      new_line.append(replacement);
      copy_from = match.end.column;
      ++match_index;
    }
    new_line.append(current_line, copy_from);

    before_changed_lines.push_back(current_line);
    if (replacement_has_break) {
      // Views: the blob copies the bytes in, so owning a string per piece first
      // was one allocation per produced line for nothing.
      for (std::string_view piece : util::SplitLineViews(new_line)) {
        after_changed_lines.push_back(piece);
      }
    } else {
      after_changed_lines.push_back(new_line);
    }
  }

  const std::size_t replacements = matches.size();
  CommitLineRangeEdit(first_changed_line, std::move(before_changed_lines),
                      std::move(after_changed_lines), before_state);
  return replacements;
}

bool TextViewport::SurroundRangeBoundaries(const SelectionRange& norm,
                                           std::string_view open,
                                           std::string_view close) {
  if (norm.start.line >= document_->lines.size() || norm.end.line >= document_->lines.size() ||
      norm.start.line > norm.end.line) {
    return false;
  }

  const ViewState before_state = CaptureViewState();
  const std::size_t first = norm.start.line;
  const std::size_t last = norm.end.line;
  const std::size_t span = last - first + 1;
  LineBlob before_lines;
  LineBlob after_lines;
  before_lines.reserve_lines(span);
  after_lines.reserve_lines(span);

  for (std::size_t line_index = first; line_index <= last; ++line_index) {
    // Composed straight into the blob from views: no per-line owned copy, and no
    // insert() shifting the tail of a string that is about to be appended anyway.
    const std::string_view original = document_->lines.LineView(line_index);
    before_lines.push_back(original);
    if (line_index == first && line_index == last) {
      after_lines.push_joined(original.substr(0, norm.start.column), open,
                              original.substr(norm.start.column,
                                              norm.end.column - norm.start.column),
                              close, original.substr(norm.end.column));
    } else if (line_index == first) {
      after_lines.push_joined(original.substr(0, norm.start.column), open,
                              original.substr(norm.start.column));
    } else if (line_index == last) {
      after_lines.push_joined(original.substr(0, norm.end.column), close,
                              original.substr(norm.end.column));
    } else {
      after_lines.push_back(original);
    }
  }

  document_->lines.ReplaceLineRange(first, span, after_lines);
  document_->dirty = true;
  document_->undo_history.ClearRedo();
  RefreshEncoding();
  InvalidateLayoutCaches();
  EnsureCursorVisible();
  const ViewState after_state = CaptureViewState();
  PushHistoryEntry(HistoryEntry{
      .start_line = first,
      .before_lines = std::move(before_lines),
      .after_lines = std::move(after_lines),
      .before_state = before_state,
      .after_state = after_state,
  });
  return true;
}

bool TextViewport::DeleteSelection() {
  const auto range = selection_range();
  if (!range.has_value()) {
    return false;
  }
  return ApplyRangeEdit(*range, "", true);
}

TextViewport::ViewState TextViewport::CaptureViewStateImpl(bool with_secondary_carets) const {
  ViewState state{
      .cursor_line = cursor_line_,
      .cursor_column = cursor_column_,
      .preferred_column = preferred_column_,
      .scroll_line = scroll_line_,
      .horizontal_scroll = horizontal_scroll_,
      .selection_anchor = selection_anchor_,
      .secondary_carets = {},
      .placeholder = document_->placeholder,
      .dirty = document_->dirty,
  };
  // The caret vector is the only heap member, and the whole point of the grouped
  // form is to never make this copy — capturing it and clearing afterwards would
  // be the same copy-then-discard shape it exists to remove.
  if (with_secondary_carets) {
    state.secondary_carets = secondary_carets_;
  }
  return state;
}

TextViewport::ViewState TextViewport::CaptureViewState() const {
  return CaptureViewStateImpl(/*with_secondary_carets=*/true);
}

TextViewport::ViewState TextViewport::CaptureViewStateForGroupedEntry() const {
  return CaptureViewStateImpl(/*with_secondary_carets=*/!document_->undo_history.IsGroupActive());
}

void TextViewport::RestoreViewState(const ViewState& state) {
  util::PerformanceTrace::Scope perf_scope("TextViewport::RestoreViewState");
  cursor_line_ = state.cursor_line;
  cursor_column_ = state.cursor_column;
  preferred_column_ = state.preferred_column;
  scroll_line_ = state.scroll_line;
  horizontal_scroll_ = state.horizontal_scroll;
  selection_anchor_ = state.selection_anchor;
  secondary_carets_ = state.secondary_carets;
  document_->placeholder = state.placeholder;
  document_->dirty = state.dirty;
}

void TextViewport::PushHistoryEntry(HistoryEntry entry, CoalesceHint hint) {
  // INVARIANT: a group frame's disjoint-range bookkeeping is expressed entirely in
  // line coordinates (start_line + before/after line vectors), so an inline child
  // has to be widened before it enters one. This is the single point where a
  // recorded entry can still be inline, which is why the widening lives here
  // rather than inside TextViewportUndoHistory (which has no buffer to read the
  // line from).
  if (entry.is_inline && document_->undo_history.IsGroupActive()) {
    WidenInlineEntryToLines(entry);
  }
  document_->undo_history.RecordEntry(std::move(entry), hint);
}


void TextViewport::BeginUndoGroup() { document_->undo_history.BeginGroup(CaptureViewState()); }

void TextViewport::EndUndoGroup() { FlushActiveUndoGroup(); }

void TextViewport::FlushActiveUndoGroup() {
  std::optional<HistoryEntry> aggregate =
      document_->undo_history.FinishActiveGroup(CaptureViewState());
  if (!aggregate.has_value()) {
    return;
  }
  // A nested group's children were already folded into the enclosing group's
  // frame by RecordEntry (which fans each child into every active frame). Pushing
  // this inner aggregate through PushHistoryEntry would re-record it into the
  // still-active outer frame and double-count those children (corrupting the final
  // undo entry's line slices). Only the outermost group commits to the stack; both
  // frames observed the identical child sequence, so discarding the inner
  // aggregate when still nested is safe.
  if (UndoGroupActive()) {
    return;
  }
  PushHistoryEntry(std::move(*aggregate));
}

void TextViewport::ApplyHistoryEntry(const HistoryEntry& entry, bool forward) {
  util::PerformanceTrace::Scope perf_scope("TextViewport::ApplyHistoryEntry");

  // A multi-range entry (TD-2026-08-06-157) is several disjoint splices. Each
  // part's buffer edit and its derived-cache update travel together, highest part
  // first: applying every splice and only then every cache update would have the
  // cache updates reasoning about indices that the earlier splices already moved.
  // Highest-first keeps every part still to come below what has been touched, so
  // its recorded index is still the right one in both structures.
  if (entry.is_multi_range()) {
    {
      util::PerformanceTrace::Scope scope("TextViewport::ApplyHistoryEntry::ApplyEntryToBuffer");
      entry.ForEachPartInApplyOrder(
          forward, [this](std::size_t start, const LineBlob& removed,
                          const LineBlob& inserted) {
            const std::size_t clamped = std::min(start, document_->lines.size());
            document_->lines.ReplaceLineRange(clamped, removed.size(), inserted);
            UpgradeEncodingForInsertedLines(inserted);
            InvalidateDerivedCaches(
                InvalidationReason::ContentEdit, clamped,
                ContentSplice{.removed = removed.size(), .inserted = inserted.size()});
            UpdateVisualColumnCacheAfterEdit(clamped, removed.size(), inserted.size(),
                                             InlineLineSplice{});
            UpdateWrappedRowsAfterEdit(clamped, removed.size(), inserted.size());
          });
      if (document_->lines.empty()) {
        document_->lines.PushBackLine("");
      }
    }
    RestoreViewState(forward ? entry.after_state : entry.before_state);
    EnsureCursorVisible();
    return;
  }

  const std::size_t start_line = std::min(entry.start_line, document_->lines.size());
  const std::size_t removed_count =
      forward ? entry.before_line_count() : entry.after_line_count();
  const std::size_t inserted_count =
      forward ? entry.after_line_count() : entry.before_line_count();
  {
    util::PerformanceTrace::Scope scope("TextViewport::ApplyHistoryEntry::ApplyEntryToBuffer");
    TextViewportUndoHistory::ApplyEntryToBuffer(document_->lines, entry, forward);
  }

  RestoreViewState(forward ? entry.after_state : entry.before_state);
  // Incremental, upgrade-only: scan just what the edit spliced in instead of
  // re-detecting the whole document's encoding on every keystroke. For an in-line
  // entry that is the delta itself, not the rebuilt line.
  if (entry.is_inline) {
    UpgradeEncodingForInsertedText(forward ? entry.inserted_text : entry.removed_text);
  } else {
    UpgradeEncodingForInsertedLines(forward ? entry.after_lines : entry.before_lines);
  }
  // Undo/redo replays a content delta starting at start_line. This is the only
  // edit path that knows the exact extent, so it is the only one that lets the
  // folding model's per-line caches resync instead of rebuilding.
  InvalidateDerivedCaches(InvalidationReason::ContentEdit, start_line,
                          ContentSplice{.removed = removed_count, .inserted = inserted_count});
  // A column-scoped entry names the byte splice it applied, which is all the
  // width table needs to update the edited line without re-reading it.
  UpdateVisualColumnCacheAfterEdit(
      start_line, removed_count, inserted_count,
      entry.is_inline
          ? InlineLineSplice{.inserted_text = forward ? entry.inserted_text : entry.removed_text,
                             .valid = true}
          : InlineLineSplice{});
  // Keep the wrapped-row table in sync incrementally so soft-wrap editing does
  // not force a full O(document) re-wrap per keystroke. A column-scoped entry
  // also names the byte the edit began at, which lets the re-wrap resume at the
  // affected row instead of at the line's first byte.
  UpdateWrappedRowsAfterEdit(start_line, removed_count, inserted_count,
                             entry.is_inline ? entry.start_column
                                             : TextLayoutCache::kNoEditColumn);
  EnsureCursorVisible();
}

std::optional<TextViewport::HistoryEntry> TextViewport::BuildRangeHistoryEntry(
    const SelectionRange& range,
    std::string_view replacement) const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::BuildRangeHistoryEntry");
  if (document_->lines.empty()) {
    return std::nullopt;
  }

  const SelectionRange normalized = NormalizeRange(range);
  const auto clamp_position = [&](TextPosition position) {
    const std::size_t line = std::min(position.line, document_->lines.size() - 1);
    // Reads a handful of bytes around the column, not the line: this runs twice
    // per edit, i.e. twice per keystroke (TD-2026-08-05-133).
    return TextPosition{
        .line = line,
        .column = ReadCaretNeighborhood(line, position.column).clamped_column,
    };
  };

  const TextPosition start = clamp_position(normalized.start);
  const TextPosition end = clamp_position(normalized.end);
  if (start.line == end.line && start.column == end.column && replacement.empty()) {
    return std::nullopt;
  }

  // The in-line case -- one line in, one line out -- is the overwhelming majority
  // of edits (every keystroke) and is the only one the line-vector entry cannot
  // express without storing the whole affected line twice. Route it to the
  // column-scoped form, whose cost is the size of the delta rather than the size
  // of the line. (TD-2026-08-05-131.)
  if (start.line == end.line && replacement.find('\n') == std::string_view::npos &&
      replacement.find('\r') == std::string_view::npos) {
    return BuildInlineHistoryEntry(start.line, start.column, end.column, replacement);
  }

  LineBlob before_lines;
  {
    util::PerformanceTrace::Scope slice_scope("TextViewport::BuildRangeHistoryEntry::SliceBefore");
    before_lines = document_->lines.SliceLinesBlob(start.line, end.line + 1);
  }
  // Views, not owned lines: `SplitLines(NormalizeLineEndings(x))` and
  // `SplitLineViews(x)` produce the same split -- '\r\n' and a lone '\r' are each
  // one break in both -- so the normalized copy of the whole replacement plus one
  // string per line bought nothing. `replacement` outlives every use below.
  const std::vector<std::string_view> replacement_lines = util::SplitLineViews(replacement);

  LineBlob after_lines;
  after_lines.reserve_lines(std::max<std::size_t>(1, replacement_lines.size()));
  // `start.column` is exact, not a clamp: ClampTextColumn already bounded it to
  // the line and snapped it to a codepoint boundary, so the prefix is that many
  // bytes and the caret arithmetic below can use it without materializing one.
  const std::size_t prefix_size = start.column;
  {
    util::PerformanceTrace::Scope compose_scope(
        "TextViewport::BuildRangeHistoryEntry::ComposeAfter");
    // Views, not owned copies, and one reserved append run per composed line.
    // Building this as `prefix + replacement + suffix` walked the affected line
    // roughly three times over: once into `prefix`, once into `suffix`, once into
    // the temporary from the first `+`, and once more into the result. On an
    // ordinary line that is noise; on the single-line file this path also has to
    // serve -- a minified bundle -- each of those is a multi-megabyte copy on
    // every keystroke. The views stay valid because nothing mutates the buffer
    // between here and the appends.
    const std::string_view prefix = document_->lines.LineView(start.line).substr(0, start.column);
    const std::string_view suffix = document_->lines.LineView(end.line).substr(end.column);
    // Size the byte buffer, not just the offset table. The appends below are onto
    // std::string's doubling curve otherwise, and on the minified-line case this
    // path exists to serve, prefix and suffix are each megabytes — so the growth
    // recopied them several times per keystroke.
    after_lines.reserve_bytes(prefix.size() + replacement.size() + suffix.size());
    if (replacement_lines.size() == 1) {
      after_lines.push_joined(prefix, std::string_view(replacement_lines.front()), suffix);
    } else {
      after_lines.push_joined(prefix, std::string_view(replacement_lines.front()));
      for (std::size_t i = 1; i + 1 < replacement_lines.size(); ++i) {
        after_lines.push_back(replacement_lines[i]);
      }
      after_lines.push_joined(std::string_view(replacement_lines.back()), suffix);
    }
  }

  // Exact no-op: the replacement reproduces the covered text byte-for-byte, so the
  // reconstructed lines equal the original slice. Reject it here so LSP WorkspaceEdits,
  // plugin apply_edits, formatters, and editor commands that return an identical
  // replacement do not bump content_revision, clear redo, invalidate layout/syntax/
  // folding caches, or notify LSP as if the file changed (TD-2026-07-17A-092).
  if (after_lines == before_lines) {
    return std::nullopt;
  }

  ViewState after_state = CaptureViewStateForGroupedEntry();
  after_state.cursor_line = start.line + after_lines.size() - 1;
  after_state.cursor_column =
      after_lines.size() == 1 ? prefix_size + replacement_lines.front().size()
                              : replacement_lines.back().size();
  // `back()` is already a view into the blob and the callee takes one; the
  // `std::string` round trip that used to sit here copied the composed line.
  after_state.preferred_column =
      TextLayout::VisualColumnForTextColumn(after_lines.back(), after_state.cursor_column, tab_size_);
  after_state.selection_anchor.reset();
  after_state.placeholder = false;
  after_state.dirty = true;

  return HistoryEntry{
      .start_line = start.line,
      .before_lines = std::move(before_lines),
      .after_lines = std::move(after_lines),
      .before_state = CaptureViewStateForGroupedEntry(),
      .after_state = after_state,
  };
}

std::optional<TextViewport::HistoryEntry> TextViewport::BuildInlineHistoryEntry(
    std::size_t line, std::size_t start_column, std::size_t end_column,
    std::string_view replacement) const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::BuildInlineHistoryEntry");
  std::string removed;
  if (end_column > start_column) {
    removed.reserve(end_column - start_column);
    // AppendTextRange, not LineView().substr: the latter materializes the WHOLE
    // line whenever it spans pieces, which after the first edit it always does.
    document_->lines.AppendTextRange(line, start_column, line, end_column, removed);
  }
  // Exact no-op (the replacement reproduces the covered text byte for byte). The
  // line form rejects these centrally for the same reasons -- LSP WorkspaceEdits,
  // plugin apply_edits and formatters that return identical text must not bump
  // content_revision or clear redo (TD-2026-07-17A-092).
  if (removed.size() == replacement.size() &&
      std::equal(removed.begin(), removed.end(), replacement.begin())) {
    return std::nullopt;
  }

  ViewState after_state = CaptureViewStateForGroupedEntry();
  after_state.cursor_line = line;
  after_state.cursor_column = start_column + replacement.size();
  // Visual column of the caret WITHOUT composing the post-edit line: the bytes
  // before `start_column` are unchanged by the edit, so walk the pre-edit line up
  // to there and continue over the replacement.
  after_state.preferred_column = TextLayout::AdvanceVisualColumnsOver(
      VisualColumnAt(line, start_column), replacement, tab_size_);
  after_state.selection_anchor.reset();
  after_state.placeholder = false;
  after_state.dirty = true;

  HistoryEntry entry;
  entry.is_inline = true;
  entry.start_line = line;
  entry.start_column = start_column;
  entry.removed_text = std::move(removed);
  entry.inserted_text.assign(replacement);
  entry.before_state = CaptureViewStateForGroupedEntry();
  entry.after_state = std::move(after_state);
  return entry;
}

void TextViewport::WidenInlineEntryToLines(HistoryEntry& entry) const {
  if (!entry.is_inline || document_->lines.empty()) {
    return;
  }
  // The edit has already been applied, so the buffer holds the AFTER line; the
  // before line is that line with the inserted bytes swapped back for the removed
  // ones. Costs one copy of the line -- paid only by grouped edits, never by a
  // keystroke.
  const std::size_t line = std::min(entry.start_line, document_->lines.size() - 1);
  std::string after_line(document_->lines.LineView(line));
  std::string before_line = after_line;
  const std::size_t column = std::min(entry.start_column, before_line.size());
  before_line.replace(column, std::min(entry.inserted_text.size(), before_line.size() - column),
                      entry.removed_text);
  entry.before_lines.clear();
  entry.before_lines.push_back(before_line);
  entry.after_lines.clear();
  entry.after_lines.push_back(after_line);
  entry.is_inline = false;
  entry.start_line = line;
  entry.start_column = 0;
  entry.removed_text.clear();
  entry.inserted_text.clear();
}

TextViewport::HistoryEntry TextViewport::BuildLineHistoryEntry(
    std::size_t start_line,
    std::size_t end_line,
    LineBlob replacement) const {
  const std::size_t clamped_start = std::min(start_line, document_->lines.size());
  const std::size_t clamped_end = std::clamp(end_line, clamped_start, document_->lines.size());

  LineBlob after_lines = std::move(replacement);
  if (after_lines.empty()) {
    after_lines.push_back("");
  }

  ViewState after_state = CaptureViewStateForGroupedEntry();
  const std::size_t total_after_lines =
      document_->lines.size() - (clamped_end - clamped_start) + after_lines.size();
  after_state.cursor_line = std::min(clamped_start, total_after_lines - 1);
  after_state.cursor_column = 0;
  after_state.preferred_column = 0;
  after_state.selection_anchor.reset();
  after_state.placeholder = false;
  after_state.dirty = true;

  return HistoryEntry{
      .start_line = clamped_start,
      .before_lines = document_->lines.SliceLinesBlob(clamped_start, clamped_end),
      .after_lines = std::move(after_lines),
      .before_state = CaptureViewStateForGroupedEntry(),
      .after_state = after_state,
  };
}

void TextViewport::CollapseSelectionAfterNoOpEdit(const SelectionRange& range,
                                                  std::string_view replacement) {
  // BuildRangeHistoryEntry rejects a replacement that reproduces the covered
  // text byte for byte, so the buffer is not dirtied and no undo entry is made
  // (TD-2026-07-17A-092). The caret still has to move when the edit IS the
  // user's own selection: typing `a` over a selected `a`, or Enter over a
  // selected line break, collapses the selection past the typed text in VS
  // Code. Left alone, the selection survived and the next keystroke replaced
  // it, so typing "ab" over a selected "a" produced "b". A programmatic edit
  // elsewhere in the buffer (an identical LSP or plugin replacement) does not
  // touch the caret, exactly as before.
  const std::optional<SelectionRange> selected = selection_range();
  const SelectionRange normalized = NormalizeRange(range);
  const TextPosition caret{cursor_line_, cursor_column_};
  const bool at_selection = selected.has_value() && selected->start == normalized.start &&
                            selected->end == normalized.end;
  const bool at_caret = !selected.has_value() && normalized.start == caret &&
                        normalized.end == caret;
  if (!at_selection && !at_caret) {
    return;
  }
  const detail::ReplacementShape shape = detail::ComputeReplacementShape(replacement);
  TextPosition landed{normalized.start.line + shape.inserted_newlines,
                      shape.inserted_newlines == 0
                          ? normalized.start.column + shape.last_segment_cols
                          : shape.last_segment_cols};
  landed.line = std::min(landed.line, document_->lines.size() - 1);
  landed.column = TextLayout::ClampTextColumn(document_->lines.LineView(landed.line), landed.column);
  cursor_line_ = landed.line;
  cursor_column_ = landed.column;
  selection_anchor_.reset();
  preferred_column_ = PreferredColumnForCaret(landed);
  EnsureCursorVisible();
}

bool TextViewport::ApplyRangeEdit(const SelectionRange& range,
                                  std::string_view replacement,
                                  bool record_undo,
                                  CoalesceHint hint) {
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.PushBackLine("");
  }

  std::optional<HistoryEntry> entry = BuildRangeHistoryEntry(range, replacement);
  if (!entry.has_value()) {
    ClearLastAppliedEdit();
    CollapseSelectionAfterNoOpEdit(range, replacement);
    return false;
  }

  ApplyHistoryEntry(*entry, true);
  SetLastAppliedEditFromEntry(*entry, true);
  if (record_undo) {
    util::PerformanceTrace::Scope scope("TextViewport::ApplyRangeEdit::PushHistoryEntry");
    // `entry` is dead after this read, so move its line vectors into the saved
    // entry instead of deep-copying them on every keystroke.
    HistoryEntry saved_entry = std::move(*entry);
    saved_entry.after_state = CaptureViewStateForGroupedEntry();
    PushHistoryEntry(std::move(saved_entry), hint);
  } else {
    document_->undo_history.ClearRedo();
  }
  return true;
}

bool TextViewport::ApplyLineEdit(std::size_t start_line,
                                 std::size_t end_line,
                                 LineBlob replacement,
                                 bool record_undo) {
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.PushBackLine("");
  }

  HistoryEntry entry = BuildLineHistoryEntry(start_line, end_line, std::move(replacement));
  // Exact no-op: the lines this edit would install are byte-for-byte identical to the
  // span it would replace, so applying it leaves the buffer unchanged. Reject it
  // centrally so line-level operations (Sort Lines on an already-sorted selection —
  // TD-2026-07-17A-091 — Toggle Comment/Move Line/Indent no-ops, direct ReplaceLines
  // callers) do not dirty the buffer, create an undo entry, or invalidate syntax/
  // folding/LSP state (TD-2026-07-17A-093). Callers already treat false as "no change".
  // after_lines/before_lines are the vectors ApplyHistoryEntry swaps, so their equality
  // is the precise "buffer unchanged" condition (empty replacements are normalized to
  // one blank line in both, matching the existing keep-at-least-one-line behavior).
  if (entry.after_lines == entry.before_lines) {
    ClearLastAppliedEdit();
    return false;
  }
  ApplyHistoryEntry(entry, true);
  SetLastAppliedEditFromEntry(entry, true);
  if (record_undo) {
    // `entry` is dead after this read; move rather than deep-copy the lines.
    HistoryEntry saved_entry = std::move(entry);
    saved_entry.after_state = CaptureViewStateForGroupedEntry();
    PushHistoryEntry(std::move(saved_entry));
  } else {
    document_->undo_history.ClearRedo();
  }
  return true;
}

}  // namespace microide::editor
