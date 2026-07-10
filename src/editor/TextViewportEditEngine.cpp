#include "editor/TextViewport.h"

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
      .changed_is_space = std::isspace(static_cast<unsigned char>(character)) != 0,
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

  const std::size_t safe_indent_width = std::max<std::size_t>(1, indent_width_);
  const std::size_t visual_column = cursor_visual_column();
  const std::size_t remainder = visual_column % safe_indent_width;
  const std::size_t spaces =
      remainder == 0 ? safe_indent_width : safe_indent_width - remainder;
  InsertText(std::string(std::max<std::size_t>(1, spaces), ' '));
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
    const std::string& line = document_->lines[cursor_line_];
    const std::size_t erase_start = TextLayout::PreviousTextColumn(line, cursor_column_);
    const CoalesceHint hint{
        .kind = CoalesceKind::DeleteBackward,
        .changed_is_space =
            erase_start < line.size() &&
            std::isspace(static_cast<unsigned char>(line[erase_start])) != 0,
    };
    (void)ApplyRangeEdit(
        SelectionRange{
            .start = TextPosition{cursor_line_, erase_start},
            .end = TextPosition{cursor_line_, cursor_column_},
        },
        "", true, hint);
    return;
  }

  if (cursor_line_ == 0) {
    return;
  }

  (void)ApplyRangeEdit(
      SelectionRange{
          .start = TextPosition{cursor_line_ - 1, document_->lines[cursor_line_ - 1].size()},
          .end = TextPosition{cursor_line_, 0},
      },
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

  const std::string& line = document_->lines[cursor_line_];
  if (cursor_column_ < line.size()) {
    const std::size_t erase_end = TextLayout::NextTextColumn(line, cursor_column_);
    const CoalesceHint hint{
        .kind = CoalesceKind::DeleteForward,
        .changed_is_space = std::isspace(static_cast<unsigned char>(line[cursor_column_])) != 0,
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

bool TextViewport::Undo() {
  util::PerformanceTrace::Scope perf_scope("TextViewport::Undo");
  if (undo_history_.IsGroupActive()) {
    FlushActiveUndoGroup();
  }
  if (!undo_history_.CanUndo()) {
    return false;
  }

  HistoryEntry entry = undo_history_.PopUndo();
  entry.after_state = CaptureViewState();
  {
    util::PerformanceTrace::Scope scope("TextViewport::Undo::ApplyHistoryEntry");
    ApplyHistoryEntry(entry, false);
  }
  {
    util::PerformanceTrace::Scope scope("TextViewport::Undo::BuildAppliedEdit");
    last_applied_edit_ = TextViewportUndoHistory::BuildAppliedEdit(entry, false);
  }
  undo_history_.PushRedo(std::move(entry));
  return true;
}

bool TextViewport::Redo() {
  util::PerformanceTrace::Scope perf_scope("TextViewport::Redo");
  if (undo_history_.IsGroupActive()) {
    FlushActiveUndoGroup();
  }
  if (!undo_history_.CanRedo()) {
    return false;
  }

  HistoryEntry entry = undo_history_.PopRedo();
  entry.before_state = CaptureViewState();
  {
    util::PerformanceTrace::Scope scope("TextViewport::Redo::ApplyHistoryEntry");
    ApplyHistoryEntry(entry, true);
  }
  {
    util::PerformanceTrace::Scope scope("TextViewport::Redo::BuildAppliedEdit");
    last_applied_edit_ = TextViewportUndoHistory::BuildAppliedEdit(entry, true);
  }
  undo_history_.PushUndo(std::move(entry));
  return true;
}

bool TextViewport::ReplaceRange(const SelectionRange& range,
                                std::string_view replacement,
                                bool record_undo) {
  return ApplyRangeEdit(range, replacement, record_undo);
}

bool TextViewport::ReplaceLines(std::size_t start_line,
                                std::size_t end_line,
                                const std::vector<std::string>& replacement,
                                bool record_undo) {
  return ApplyLineEdit(start_line, end_line, replacement, record_undo);
}

std::size_t TextViewport::ReplaceAll(std::string_view needle, std::string_view replacement) {
  if (needle.empty() || document_->lines.empty()) {
    return 0;
  }

  const ViewState before_state = CaptureViewState();
  const std::string lowered_needle = util::ToLowerAscii(needle);
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
  std::vector<std::string> before_changed_lines;
  std::vector<std::string> after_changed_lines;

  // Build the final content of the changed span in one pass, then apply it with a
  // single ReplaceLineRange so InvalidateDerivedCaches / RefreshEncoding run once
  // and multi-line replacements splice in as real lines.
  std::string new_line;
  std::string lowered_line;  // reused across lines to avoid a per-line allocation
  for (std::size_t line_index = 0; line_index < document_->lines.size(); ++line_index) {
    std::string current_line(document_->lines.LineView(line_index));
    util::ToLowerAsciiInto(current_line, lowered_line);
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
        std::string gap_line(document_->lines.LineView(gap));
        before_changed_lines.push_back(gap_line);
        after_changed_lines.push_back(std::move(gap_line));
      }
    }
    before_changed_lines.push_back(std::move(current_line));
    if (replacement_has_break) {
      for (std::string& piece : util::SplitLines(new_line)) {
        after_changed_lines.push_back(std::move(piece));
      }
    } else {
      after_changed_lines.push_back(new_line);
    }
    last_changed_line = line_index;
  }

  if (replacements > 0) {
    document_->lines.ReplaceLineRange(first_changed_line, before_changed_lines.size(),
                                      after_changed_lines);
    document_->dirty = true;
    undo_history_.ClearRedo();
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
  return replacements;
}

bool TextViewport::DeleteSelection() {
  const auto range = selection_range();
  if (!range.has_value()) {
    return false;
  }
  return ApplyRangeEdit(*range, "", true);
}

TextViewport::ViewState TextViewport::CaptureViewState() const {
  return ViewState{
      .cursor_line = cursor_line_,
      .cursor_column = cursor_column_,
      .preferred_column = preferred_column_,
      .scroll_line = scroll_line_,
      .horizontal_scroll = horizontal_scroll_,
      .selection_anchor = selection_anchor_,
      .secondary_carets = secondary_carets_,
      .placeholder = document_->placeholder,
      .dirty = document_->dirty,
  };
}

void TextViewport::RestoreViewState(const ViewState& state) {
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
  undo_history_.RecordEntry(std::move(entry), document_->lines, hint);
}

void TextViewport::PushHistoryEntryDirect(HistoryEntry entry) {
  undo_history_.RecordEntryDirect(std::move(entry));
}

void TextViewport::BeginUndoGroup() { undo_history_.BeginGroup(CaptureViewState()); }

void TextViewport::EndUndoGroup() { FlushActiveUndoGroup(); }

void TextViewport::FlushActiveUndoGroup() {
  std::optional<HistoryEntry> aggregate =
      undo_history_.FinishActiveGroup(document_->lines, CaptureViewState());
  if (!aggregate.has_value()) {
    return;
  }
  PushHistoryEntry(std::move(*aggregate));
}

void TextViewport::ApplyHistoryEntry(const HistoryEntry& entry, bool forward) {
  const std::size_t start_line = std::min(entry.start_line, document_->lines.size());
  const std::size_t removed_count = forward ? entry.before_lines.size() : entry.after_lines.size();
  const auto& inserted_lines = forward ? entry.after_lines : entry.before_lines;
  TextViewportUndoHistory::ApplyEntryToBuffer(document_->lines, entry, forward);

  RestoreViewState(forward ? entry.after_state : entry.before_state);
  // Incremental, upgrade-only: scan just the inserted lines instead of
  // re-detecting the whole document's encoding on every keystroke.
  UpgradeEncodingForInsertedLines(inserted_lines);
  // Undo/redo replays a content delta starting at start_line.
  InvalidateDerivedCaches(InvalidationReason::ContentEdit, start_line);
  UpdateVisualColumnCacheAfterEdit(start_line, removed_count, inserted_lines);
  // Keep the wrapped-row table in sync incrementally so soft-wrap editing does
  // not force a full O(document) re-wrap per keystroke.
  UpdateWrappedRowsAfterEdit(start_line, removed_count, inserted_lines);
  EnsureCursorVisible();
}

std::optional<TextViewport::HistoryEntry> TextViewport::BuildRangeHistoryEntry(
    const SelectionRange& range,
    std::string_view replacement) const {
  if (document_->lines.empty()) {
    return std::nullopt;
  }

  const SelectionRange normalized = NormalizeRange(range);
  const auto clamp_position = [&](TextPosition position) {
    const std::size_t line = std::min(position.line, document_->lines.size() - 1);
    return TextPosition{
        .line = line,
        .column = TextLayout::ClampTextColumn(document_->lines[line], position.column),
    };
  };

  const TextPosition start = clamp_position(normalized.start);
  const TextPosition end = clamp_position(normalized.end);
  if (start.line == end.line && start.column == end.column && replacement.empty()) {
    return std::nullopt;
  }

  std::vector<std::string> before_lines =
      document_->lines.SliceLines(start.line, end.line + 1);
  const std::vector<std::string> replacement_lines =
      util::SplitLines(util::NormalizeLineEndings(replacement));

  std::vector<std::string> after_lines;
  after_lines.reserve(std::max<std::size_t>(1, replacement_lines.size()));
  const std::string prefix = document_->lines[start.line].substr(0, start.column);
  const std::string suffix = document_->lines[end.line].substr(end.column);
  if (replacement_lines.size() == 1) {
    after_lines.push_back(prefix + replacement_lines.front() + suffix);
  } else {
    after_lines.push_back(prefix + replacement_lines.front());
    after_lines.insert(after_lines.end(), replacement_lines.begin() + 1,
                       replacement_lines.end() - 1);
    after_lines.push_back(replacement_lines.back() + suffix);
  }

  ViewState after_state = CaptureViewState();
  after_state.cursor_line = start.line + after_lines.size() - 1;
  after_state.cursor_column =
      after_lines.size() == 1 ? prefix.size() + replacement_lines.front().size()
                              : replacement_lines.back().size();
  after_state.preferred_column = TextLayout::VisualColumnForTextColumn(
      after_lines.back(), after_state.cursor_column, tab_size_);
  after_state.selection_anchor.reset();
  after_state.placeholder = false;
  after_state.dirty = true;

  return HistoryEntry{
      .start_line = start.line,
      .before_lines = std::move(before_lines),
      .after_lines = std::move(after_lines),
      .before_state = CaptureViewState(),
      .after_state = after_state,
  };
}

TextViewport::HistoryEntry TextViewport::BuildLineHistoryEntry(
    std::size_t start_line,
    std::size_t end_line,
    const std::vector<std::string>& replacement) const {
  const std::size_t clamped_start = std::min(start_line, document_->lines.size());
  const std::size_t clamped_end = std::clamp(end_line, clamped_start, document_->lines.size());

  std::vector<std::string> after_lines = replacement;
  if (after_lines.empty()) {
    after_lines.push_back("");
  }

  ViewState after_state = CaptureViewState();
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
      .before_lines = document_->lines.SliceLines(clamped_start, clamped_end),
      .after_lines = std::move(after_lines),
      .before_state = CaptureViewState(),
      .after_state = after_state,
  };
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
    last_applied_edit_.reset();
    return false;
  }

  ApplyHistoryEntry(*entry, true);
  last_applied_edit_ = TextViewportUndoHistory::BuildAppliedEdit(*entry, true);
  if (record_undo) {
    // `entry` is dead after this read, so move its line vectors into the saved
    // entry instead of deep-copying them on every keystroke.
    HistoryEntry saved_entry = std::move(*entry);
    saved_entry.after_state = CaptureViewState();
    PushHistoryEntry(std::move(saved_entry), hint);
  } else {
    undo_history_.ClearRedo();
  }
  return true;
}

bool TextViewport::ApplyLineEdit(std::size_t start_line,
                                 std::size_t end_line,
                                 const std::vector<std::string>& replacement,
                                 bool record_undo) {
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.PushBackLine("");
  }

  HistoryEntry entry = BuildLineHistoryEntry(start_line, end_line, replacement);
  ApplyHistoryEntry(entry, true);
  last_applied_edit_ = TextViewportUndoHistory::BuildAppliedEdit(entry, true);
  if (record_undo) {
    // `entry` is dead after this read; move rather than deep-copy the lines.
    HistoryEntry saved_entry = std::move(entry);
    saved_entry.after_state = CaptureViewState();
    PushHistoryEntry(std::move(saved_entry));
  } else {
    undo_history_.ClearRedo();
  }
  return true;
}

}  // namespace microide::editor
