#include "editor/TextViewport.h"
#include "editor/TextViewportInternal.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextLayout.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"

namespace microide::editor {


void TextViewport::InsertCharacter(char character) {
  SyncCaretsIfDocumentChangedElsewhere();

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
  SyncCaretsIfDocumentChangedElsewhere();

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
  SyncCaretsIfDocumentChangedElsewhere();

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
  const std::string newline_text =
      "\n" + AutoIndentForNewline(range.start.line, range.start.column);
  (void)ApplyRangeEdit(range, newline_text, true);
}

void TextViewport::InsertTab() {
  SyncCaretsIfDocumentChangedElsewhere();

  if (!soft_tabs_) {
    InsertCharacter('\t');
    return;
  }

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
  if (target >= column) {
    return std::nullopt;
  }
  return target;
}

void TextViewport::Backspace() {
  SyncCaretsIfDocumentChangedElsewhere();

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
    const CaretNeighborhood at = ReadCaretNeighborhood(cursor_line_, cursor_column_);
    const bool prev_is_space = at.has_prev && (at.prev_char == ' ' || at.prev_char == '\t');
    const std::size_t erase_start =
        prev_is_space
            ? IndentStopBackspaceStart(cursor_line_, at.clamped_column).value_or(at.prev_column)
            : (at.has_prev ? at.prev_column : 0);
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
  SyncCaretsIfDocumentChangedElsewhere();

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

  if (const auto selected = selection_range(); selected.has_value()) {
    (void)ApplyRangeEdit(*selected, "", true);
    return;
  }

  const TextPosition caret{cursor_line_, cursor_column_};
  const TextPosition target = WordTargetForCaret(caret, direction, true);
  if (target.line == caret.line && target.column == caret.column) {
    return;
  }
  (void)ApplyRangeEdit(direction < 0 ? SelectionRange{target, caret} : SelectionRange{caret, target},
                       "", true);
}

void TextViewport::DeleteForward() {
  SyncCaretsIfDocumentChangedElsewhere();

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

  const std::string_view line = document_->lines.LineView(cursor_line_);
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
  if (read_only_) {
    ++refused_edits_;
    return false;
  }
  SyncCaretsIfDocumentChangedElsewhere();

  util::PerformanceTrace::Scope perf_scope(redo ? "TextViewport::Redo" : "TextViewport::Undo");
  if (document_->undo_history.IsGroupActive()) {
    FlushActiveUndoGroup();
  }
  if (redo ? !document_->undo_history.CanRedo() : !document_->undo_history.CanUndo()) {
    return false;
  }

  HistoryEntry entry = redo ? document_->undo_history.PopRedo() : document_->undo_history.PopUndo();
  const std::uint64_t edit_base_revision = document_->content_revision;
  (redo ? entry.before_state : entry.after_state) = CaptureViewState();
  {
    util::PerformanceTrace::Scope scope(redo ? "TextViewport::Redo::ApplyHistoryEntry"
                                             : "TextViewport::Undo::ApplyHistoryEntry");
    ApplyHistoryEntry(entry, redo);
  }
  ClampCaretSetToDocument();
  DedupeSecondaryCaretsAgainstPrimary();
  {
    util::PerformanceTrace::Scope scope(redo ? "TextViewport::Redo::BuildAppliedEdit"
                                             : "TextViewport::Undo::BuildAppliedEdit");
    SetLastAppliedEditFromEntry(entry, redo, edit_base_revision);
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

bool TextViewport::Undo() {
  const bool was_dirty = document_ != nullptr && document_->dirty;
  const bool applied = ApplyHistoryStep(false);
  if (applied && was_dirty && !document_->dirty) {
    TraceBufferIo("undo-to-clean");
  }
  return applied;
}

bool TextViewport::Redo() {
  const bool was_dirty = document_ != nullptr && document_->dirty;
  const bool applied = ApplyHistoryStep(true);
  if (applied && was_dirty && !document_->dirty) {
    TraceBufferIo("redo-to-clean");
  }
  return applied;
}

bool TextViewport::ReplaceRange(const SelectionRange& range,
                                std::string_view replacement,
                                bool record_undo) {
  return ApplyRangeEdit(range, replacement, record_undo);
}

bool TextViewport::ReplaceLines(std::size_t start_line,
                                std::size_t end_line,
                                std::vector<std::string> replacement,
                                bool record_undo) {
  return ApplyLineEdit(start_line, end_line, LineBlob(replacement), record_undo);
}

bool TextViewport::ReplaceLines(std::size_t start_line,
                                std::size_t end_line,
                                LineBlob replacement,
                                bool record_undo) {
  return ApplyLineEdit(start_line, end_line, std::move(replacement), record_undo);
}

void TextViewport::CarryCaretsThroughReplacements(const std::vector<SelectionRange>& matches,
                                                  std::string_view replacement) {
  AppliedEdit each{.range_before = {}, .replacement_text = std::string(replacement)};
  const auto carry = [&matches, &each](TextPosition position) {
    for (auto match = matches.rbegin(); match != matches.rend(); ++match) {
      each.range_before = *match;
      position = MapPositionThroughEdit(position, each);
    }
    return position;
  };
  const TextPosition caret = carry(TextPosition{cursor_line_, cursor_column_});
  cursor_line_ = caret.line;
  cursor_column_ = caret.column;
  preferred_column_ = caret.column;
  if (selection_anchor_.has_value()) {
    selection_anchor_ = carry(*selection_anchor_);
  }
  for (TextViewportUndoHistory::SecondaryCaret& other : secondary_carets_) {
    other.position = carry(other.position);
    other.preferred_column = other.position.column;
    if (other.selection_anchor.has_value()) {
      other.selection_anchor = carry(*other.selection_anchor);
    }
  }
  PruneCoincidentSecondaryCarets();
}

std::size_t TextViewport::ReplaceAll(std::string_view needle, std::string_view replacement) {
  if (needle.empty() || document_->lines.empty()) {
    return 0;
  }

  const ViewState before_state = CaptureViewState();
  const std::string lowered_needle = util::Utf8CaseFold(needle);
  const bool replacement_has_break =
      replacement.find('\n') != std::string_view::npos ||
      replacement.find('\r') != std::string_view::npos;
  std::size_t replacements = 0;
  std::size_t first_changed_line = document_->lines.size();
  std::size_t last_changed_line = 0;
  LineBlob before_changed_lines;
  LineBlob after_changed_lines;

  std::string new_line;
  std::string lowered_line;
  std::vector<SelectionRange> replaced;
  for (std::size_t line_index = 0; line_index < document_->lines.size(); ++line_index) {
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
      replaced.push_back(SelectionRange{{line_index, offset}, {line_index, copy_from}});
      ++replacements;
      offset = lowered_line.find(lowered_needle, copy_from);
    }
    new_line.append(current_line, copy_from);

    if (first_changed_line == document_->lines.size()) {
      first_changed_line = line_index;
    } else {
      for (std::size_t gap = last_changed_line + 1; gap < line_index; ++gap) {
        const std::string_view gap_line = document_->lines.LineView(gap);
        before_changed_lines.push_back(gap_line);
        after_changed_lines.push_back(gap_line);
      }
    }
    before_changed_lines.push_back(current_line);
    if (replacement_has_break) {
      for (std::string_view piece : util::SplitLineViews(new_line)) {
        after_changed_lines.push_back(piece);
      }
    } else {
      after_changed_lines.push_back(new_line);
    }
    last_changed_line = line_index;
  }

  if (replacements > 0) {
    CarryCaretsThroughReplacements(replaced, replacement);
    CommitLineRangeEdit(first_changed_line, std::move(before_changed_lines),
                        std::move(after_changed_lines), before_state);
  }
  return replacements;
}

void TextViewport::CommitLineRangeEdit(std::size_t first_changed_line,
                                       LineBlob before_changed_lines,
                                       LineBlob after_changed_lines,
                                       const ViewState& before_state) {
  ClearLastAppliedEdit();
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
      for (std::string_view piece : util::SplitLineViews(new_line)) {
        after_changed_lines.push_back(piece);
      }
    } else {
      after_changed_lines.push_back(new_line);
    }
  }

  CarryCaretsThroughReplacements(matches, replacement);

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

  CommitLineRangeEdit(first, std::move(before_lines), std::move(after_lines), before_state);
  return true;
}

bool TextViewport::DeleteSelection() {
  const auto range = selection_range();
  if (!range.has_value()) {
    return false;
  }
  if (secondary_carets_.empty()) {
    return ApplyRangeEdit(*range, "", true);
  }
  std::vector<TextViewportUndoHistory::SecondaryCaret> others = std::move(secondary_carets_);
  secondary_carets_.clear();
  if (!ApplyRangeEdit(*range, "", true) || !last_applied_edit_.has_value()) {
    secondary_carets_ = std::move(others);
    return false;
  }
  for (TextViewportUndoHistory::SecondaryCaret& caret : others) {
    caret.position = MapPositionThroughEdit(caret.position, *last_applied_edit_);
    if (caret.selection_anchor.has_value()) {
      caret.selection_anchor = MapPositionThroughEdit(*caret.selection_anchor, *last_applied_edit_);
    }
  }
  secondary_carets_ = std::move(others);
  DedupeSecondaryCaretsAgainstPrimary();
  return true;
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
  if (with_secondary_carets) {
    state.secondary_carets = secondary_carets_;
    util::AddPerformanceCounter(util::PerfCounterId::EditorHistoryCaretCopies, secondary_carets_.size());
  }
  return state;
}

TextViewport::ViewState TextViewport::CaptureViewState() const {
  return CaptureViewStateImpl(true);
}

TextViewport::ViewState TextViewport::CaptureViewStateForGroupedEntry() const {
  const bool entry_state_is_discarded =
      document_->undo_history.IsGroupActive() || suppress_entry_caret_capture_;
  return CaptureViewStateImpl(!entry_state_is_discarded);
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
  util::AddPerformanceCounter(util::PerfCounterId::EditorHistoryCaretCopies, state.secondary_carets.size());
  document_->placeholder = state.placeholder;
  document_->dirty = state.dirty;
}

void TextViewport::PushHistoryEntry(HistoryEntry entry, CoalesceHint hint) {
  if (entry.is_inline && document_->undo_history.IsGroupActive()) {
    WidenInlineEntryToLines(entry);
  }
  document_->undo_history.RecordEntry(std::move(entry), hint);
}


void TextViewport::BeginUndoGroup() {
  if (!UndoGroupActive()) {
    undo_group_base_revision_ = document_->content_revision;
  }
  document_->undo_history.BeginGroup(CaptureViewState());
}

void TextViewport::EndUndoGroup() { FlushActiveUndoGroup(); }

void TextViewport::FlushActiveUndoGroup() {
  std::optional<HistoryEntry> aggregate =
      document_->undo_history.FinishActiveGroup(CaptureViewState());
  if (!aggregate.has_value()) {
    return;
  }
  if (UndoGroupActive()) {
    return;
  }
  SetLastAppliedEditFromEntry(*aggregate, true, undo_group_base_revision_);
  PushHistoryEntry(std::move(*aggregate));
}

void TextViewport::ApplyHistoryEntry(const HistoryEntry& entry, bool forward) {
  util::PerformanceTrace::Scope perf_scope("TextViewport::ApplyHistoryEntry");

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
  if (entry.line_ending_after.has_value() && entry.line_ending_before.has_value()) {
    document_->line_ending = forward ? *entry.line_ending_after : *entry.line_ending_before;
    document_->mixed_line_endings = false;
  }

  RestoreViewState(forward ? entry.after_state : entry.before_state);
  if (entry.is_inline) {
    UpgradeEncodingForInsertedText(forward ? entry.inserted_text : entry.removed_text);
  } else {
    UpgradeEncodingForInsertedLines(forward ? entry.after_lines : entry.before_lines);
  }
  InvalidateDerivedCaches(
      InvalidationReason::ContentEdit, start_line,
      ContentSplice{.removed = removed_count,
                    .inserted = inserted_count,
                    .edit_column = entry.is_inline ? entry.start_column : TextLayoutCache::kNoEditColumn});
  UpdateVisualColumnCacheAfterEdit(
      start_line, removed_count, inserted_count,
      entry.is_inline
          ? InlineLineSplice{.inserted_text = forward ? entry.inserted_text : entry.removed_text,
                             .valid = true}
          : InlineLineSplice{});
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

  if (start.line == end.line && replacement.find('\n') == std::string_view::npos &&
      replacement.find('\r') == std::string_view::npos) {
    return BuildInlineHistoryEntry(start.line, start.column, end.column, replacement);
  }

  LineBlob before_lines;
  {
    util::PerformanceTrace::Scope slice_scope("TextViewport::BuildRangeHistoryEntry::SliceBefore");
    before_lines = document_->lines.SliceLinesBlob(start.line, end.line + 1);
  }
  const std::vector<std::string_view> replacement_lines = util::SplitLineViews(replacement);

  LineBlob after_lines;
  after_lines.reserve_lines(std::max<std::size_t>(1, replacement_lines.size()));
  const std::size_t prefix_size = start.column;
  {
    util::PerformanceTrace::Scope compose_scope(
        "TextViewport::BuildRangeHistoryEntry::ComposeAfter");
    const std::string_view prefix = document_->lines.LineView(start.line).substr(0, start.column);
    const std::string_view suffix = document_->lines.LineView(end.line).substr(end.column);
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

  if (after_lines == before_lines) {
    return std::nullopt;
  }

  ViewState after_state = CaptureViewStateForGroupedEntry();
  after_state.cursor_line = start.line + after_lines.size() - 1;
  after_state.cursor_column =
      after_lines.size() == 1 ? prefix_size + replacement_lines.front().size()
                              : replacement_lines.back().size();
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
    document_->lines.AppendTextRange(line, start_column, line, end_column, removed);
  }
  if (removed.size() == replacement.size() &&
      std::equal(removed.begin(), removed.end(), replacement.begin())) {
    return std::nullopt;
  }

  ViewState after_state = CaptureViewStateForGroupedEntry();
  after_state.cursor_line = line;
  after_state.cursor_column = start_column + replacement.size();
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
  if (read_only_) {
    ++refused_edits_;
    return false;
  }
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

  const std::uint64_t edit_base_revision = document_->content_revision;
  ApplyHistoryEntry(*entry, true);
  SetLastAppliedEditFromEntry(*entry, true, edit_base_revision);
  if (record_undo) {
    util::PerformanceTrace::Scope scope("TextViewport::ApplyRangeEdit::PushHistoryEntry");
    HistoryEntry saved_entry = std::move(*entry);
    saved_entry.after_state = CaptureViewStateForGroupedEntry();
    PushHistoryEntry(std::move(saved_entry), hint);
  } else {
    document_->undo_history.ClearRedo();
  }
  return true;
}

bool TextViewport::SetLineEnding(LineEnding ending) {
  if (read_only_) {
    ++refused_edits_;
    return false;
  }
  EnsureDocument();
  if ((document_->line_ending == ending && !document_->mixed_line_endings) ||
      document_->undo_history.IsGroupActive()) {
    return false;
  }
  HistoryEntry entry;
  entry.line_ending_before = document_->line_ending;
  entry.line_ending_after = ending;
  entry.before_state = CaptureViewState();
  document_->line_ending = ending;
  document_->mixed_line_endings = false;
  document_->dirty = true;
  InvalidateDerivedCaches(InvalidationReason::ContentEdit);
  ClearLastAppliedEdit();
  entry.after_state = CaptureViewState();
  document_->undo_history.ClearRedo();
  PushHistoryEntry(std::move(entry));
  return true;
}

bool TextViewport::ApplyLineEdit(std::size_t start_line,
                                 std::size_t end_line,
                                 LineBlob replacement,
                                 bool record_undo) {
  if (read_only_) {
    ++refused_edits_;
    return false;
  }
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.PushBackLine("");
  }

  HistoryEntry entry = BuildLineHistoryEntry(start_line, end_line, std::move(replacement));
  if (entry.after_lines == entry.before_lines) {
    ClearLastAppliedEdit();
    return false;
  }
  const std::uint64_t edit_base_revision = document_->content_revision;
  ApplyHistoryEntry(entry, true);
  SetLastAppliedEditFromEntry(entry, true, edit_base_revision);
  if (record_undo) {
    HistoryEntry saved_entry = std::move(entry);
    saved_entry.after_state = CaptureViewStateForGroupedEntry();
    PushHistoryEntry(std::move(saved_entry));
  } else {
    document_->undo_history.ClearRedo();
  }
  return true;
}

}
