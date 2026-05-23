#include "editor/TextViewport.h"
#include "editor/TextViewportInternal.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextLayout.h"
#include "util/StringUtil.h"

// Language-pair behavior for TextViewport: auto-close, surround, skip-over-close,
// dedent-on-close, brace-split newline, smart-indent newline, and the multi-caret
// pair-insert path. Split out from TextViewport.cpp on 2026-05-18 to relieve the
// single-file ownership concentration tracked as item #15 in
// dev-docs/project/known-tech-debt.md. No header / API change: these are the same
// `TextViewport` member functions, defined in a sibling translation unit.
//
// Shared file-scope helpers used across TUs live in `TextViewportInternal.h`.

namespace microide::editor {

namespace {

void PostSurroundInnerSelection(const LanguagePair& pair,
                                const std::string& inner,
                                std::size_t start_line,
                                const std::string& first_line_prefix,
                                TextPosition* anchor,
                                TextPosition* cursor) {
  const std::vector<std::string> inner_lines =
      util::SplitLines(util::NormalizeLineEndings(inner));
  if (inner_lines.empty()) {
    return;
  }
  *anchor = TextPosition{start_line, first_line_prefix.size() + pair.open.size()};
  const std::size_t last_line = start_line + inner_lines.size() - 1;
  // For single-line surround the inner text shares the line with the open
  // delimiter and any leading prefix, so the cursor column must be offset by
  // both. Multi-line surround pushes the closer onto the original last line
  // with no extra offset.
  const std::size_t single_line_offset =
      (last_line == start_line) ? first_line_prefix.size() + pair.open.size() : 0;
  *cursor = TextPosition{last_line, single_line_offset + inner_lines.back().size()};
}

bool IsIndentCharacter(char c) {
  return c == ' ' || c == '\t';
}

bool LineIsIndentOnly(const std::string& line) {
  return std::all_of(line.begin(), line.end(),
                     [](char c) { return IsIndentCharacter(c); });
}

std::string_view TrimmedRightView(const std::string& line) {
  std::size_t end = line.size();
  while (end > 0 && IsIndentCharacter(line[end - 1])) {
    --end;
  }
  return std::string_view(line.data(), end);
}

const LanguagePair* FindAutoCloseOpener(const LanguageContractView& view, char ch) {
  for (const auto& pair : view.auto_close_pairs) {
    if (pair.open.size() == 1 && pair.open[0] == ch) {
      return &pair;
    }
  }
  return nullptr;
}

const LanguagePair* FindAutoCloseCloser(const LanguageContractView& view, char ch) {
  for (const auto& pair : view.auto_close_pairs) {
    if (pair.close.size() == 1 && pair.close[0] == ch) {
      return &pair;
    }
  }
  return nullptr;
}

const LanguagePair* FindSurroundOpener(const LanguageContractView& view, char ch) {
  for (const auto& pair : view.surround_pairs) {
    if (pair.open.size() == 1 && pair.open[0] == ch) {
      return &pair;
    }
  }
  return nullptr;
}

bool ShouldAutoCloseAtNext(const std::string& line, std::size_t column,
                           const LanguageContractView& view) {
  if (column >= line.size()) {
    return true;
  }
  const char next = line[column];
  if (next == ' ' || next == '\t') {
    return true;
  }
  if (FindAutoCloseCloser(view, next) != nullptr) {
    return true;
  }
  if (next == ')' || next == ']' || next == '}' || next == '>' || next == ',' ||
      next == ';' || next == ':') {
    return true;
  }
  return false;
}

bool DedentOnCloseTokenMatches(const LanguageContractView& view, char ch) {
  for (const auto& token : view.dedent_on_close_chars) {
    if (token.size() == 1 && token[0] == ch) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::string TextViewport::AutoIndentForNewline(std::size_t line, std::size_t column) const {
  if (line >= document_->lines.size()) {
    return {};
  }

  const std::string& current_line = document_->lines[line];
  const std::size_t clamped_column = TextLayout::ClampTextColumn(current_line, column);
  const auto first_non_indent = std::find_if(
      current_line.begin(), current_line.end(),
      [](char c) { return !IsIndentCharacter(c); });

  std::string base_indent;
  if (first_non_indent != current_line.end()) {
    const std::size_t indent_columns = std::min<std::size_t>(
        static_cast<std::size_t>(first_non_indent - current_line.begin()),
        clamped_column);
    base_indent.assign(current_line, 0, indent_columns);
  }

  if (!lc_view_.smart_indent_enabled || lc_view_.indent_after_open_patterns.empty()) {
    return base_indent;
  }

  const std::string_view trimmed = TrimmedRightView(current_line);
  if (clamped_column < trimmed.size()) {
    return base_indent;
  }
  for (const std::string& pattern : lc_view_.indent_after_open_patterns) {
    if (pattern.empty()) {
      continue;
    }
    if (trimmed.size() >= pattern.size() &&
        trimmed.substr(trimmed.size() - pattern.size()) == pattern) {
      return base_indent + IndentUnit();
    }
  }
  return base_indent;
}

std::string TextViewport::IndentUnit() const {
  if (soft_tabs_) {
    return std::string(std::max<std::size_t>(1, indent_width_), ' ');
  }
  return "\t";
}

bool TextViewport::TryAutoCloseInsert(char ch) {
  if (!lc_view_.auto_close_enabled || has_selection()) {
    return false;
  }
  const auto* pair = FindAutoCloseOpener(lc_view_, ch);
  if (pair == nullptr || pair->close.empty()) {
    return false;
  }
  if (cursor_line_ >= document_->lines.size()) {
    return false;
  }
  const std::string& current_line = document_->lines[cursor_line_];
  const std::size_t clamped_column = TextLayout::ClampTextColumn(current_line, cursor_column_);
  if (InInsertionSuppressedScope(cursor_line_, clamped_column)) {
    return false;
  }
  if (!ShouldAutoCloseAtNext(current_line, clamped_column, lc_view_)) {
    return false;
  }
  // Same-character pairs (quotes): avoid stacking when the previous char is
  // already the same quote (typing inside a comment/string we can't detect
  // semantically) -- simple heuristic.
  if (pair->open == pair->close && clamped_column > 0 &&
      current_line[clamped_column - 1] == ch) {
    return false;
  }

  const SelectionRange range = SelectionRange{
      TextPosition{cursor_line_, clamped_column},
      TextPosition{cursor_line_, clamped_column},
  };
  const std::string replacement = pair->open + pair->close;
  if (!ApplyRangeEdit(range, replacement, true)) {
    return false;
  }
  // Position caret between open and close (open is one byte; we already
  // restricted to single-character pairs above for FindAutoCloseOpener match).
  cursor_column_ = clamped_column + pair->open.size();
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  EnsureCursorVisible();
  return true;
}

bool TextViewport::TrySurroundInsert(char ch) {
  if (!lc_view_.surround_enabled) {
    return false;
  }
  const auto sel = selection_range();
  if (!sel.has_value()) {
    return false;
  }
  const auto* pair = FindSurroundOpener(lc_view_, ch);
  if (pair == nullptr || pair->open.empty() || pair->close.empty()) {
    return false;
  }
  const SelectionRange norm = NormalizeRange(*sel);
  if (!detail::ValidateRangeColumns(document_->lines, norm)) {
    return false;
  }
  if (InInsertionSuppressedScope(norm.start.line, norm.start.column)) {
    return false;
  }

  const std::string inner = detail::TextBetweenLines(document_->lines, norm);
  const std::string replacement = pair->open + inner + pair->close;
  const std::string first_prefix = document_->lines[norm.start.line].substr(0, norm.start.column);
  TextPosition inner_anchor{};
  TextPosition inner_cursor{};
  PostSurroundInnerSelection(*pair, inner, norm.start.line, first_prefix, &inner_anchor,
                             &inner_cursor);

  if (!ApplyRangeEdit(norm, replacement, true)) {
    return false;
  }
  selection_anchor_ = inner_anchor;
  cursor_line_ = inner_cursor.line;
  cursor_column_ = inner_cursor.column;
  preferred_column_ = PreferredColumnForCaret(inner_cursor);
  EnsureCursorVisible();
  return true;
}

bool TextViewport::TrySkipOverClose(char ch) {
  if (!lc_view_.auto_close_enabled || has_selection()) {
    return false;
  }
  if (cursor_line_ >= document_->lines.size()) {
    return false;
  }
  const std::string& current_line = document_->lines[cursor_line_];
  const std::size_t clamped_column = TextLayout::ClampTextColumn(current_line, cursor_column_);
  if (clamped_column >= current_line.size()) {
    return false;
  }
  if (current_line[clamped_column] != ch) {
    return false;
  }
  if (FindAutoCloseCloser(lc_view_, ch) == nullptr) {
    return false;
  }
  cursor_column_ = clamped_column + 1;
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  EnsureCursorVisible();
  return true;
}

bool TextViewport::MaybeDedentOnClose(char ch) {
  if (!lc_view_.smart_indent_enabled) {
    return false;
  }
  if (has_selection()) {
    return false;
  }
  if (!DedentOnCloseTokenMatches(lc_view_, ch)) {
    return false;
  }
  if (cursor_line_ >= document_->lines.size()) {
    return false;
  }
  const std::string& current_line = document_->lines[cursor_line_];
  if (!LineIsIndentOnly(current_line)) {
    return false;
  }
  if (cursor_column_ != current_line.size()) {
    return false;
  }
  const std::string unit = IndentUnit();
  if (current_line.size() < unit.size()) {
    return false;
  }
  if (current_line.compare(current_line.size() - unit.size(), unit.size(), unit) != 0) {
    return false;
  }
  const SelectionRange erase_range = SelectionRange{
      TextPosition{cursor_line_, current_line.size() - unit.size()},
      TextPosition{cursor_line_, current_line.size()},
  };
  return ApplyRangeEdit(erase_range, "", true);
}

bool TextViewport::TryInsertNewlineSplitBraces() {
  if (has_selection() || has_multiple_carets()) {
    return false;
  }
  if (!lc_view_.auto_close_enabled) {
    return false;
  }
  if (cursor_line_ >= document_->lines.size()) {
    return false;
  }
  const std::string& current_line = document_->lines[cursor_line_];
  const std::size_t column = TextLayout::ClampTextColumn(current_line, cursor_column_);
  if (column == 0 || column >= current_line.size()) {
    return false;
  }
  const char prev = current_line[column - 1];
  const char next = current_line[column];
  const auto* opener = FindAutoCloseOpener(lc_view_, prev);
  if (opener == nullptr || opener->close.size() != 1 || opener->close[0] != next) {
    return false;
  }
  const std::string base_indent = AutoIndentForNewline(cursor_line_, column);
  const std::string unit = IndentUnit();
  std::string inner_indent;
  if (lc_view_.smart_indent_enabled) {
    inner_indent = base_indent + unit;
  } else {
    inner_indent = base_indent + unit;
  }
  const std::string replacement = std::string("\n") + inner_indent + "\n" + base_indent;
  const std::size_t opener_line = cursor_line_;
  const SelectionRange range = SelectionRange{
      TextPosition{cursor_line_, column},
      TextPosition{cursor_line_, column},
  };
  if (!ApplyRangeEdit(range, replacement, true)) {
    return false;
  }
  cursor_line_ = opener_line + 1;
  if (cursor_line_ < document_->lines.size()) {
    cursor_column_ = std::min(inner_indent.size(), document_->lines[cursor_line_].size());
  } else {
    cursor_column_ = 0;
  }
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  EnsureCursorVisible();
  return true;
}

bool TextViewport::InInsertionSuppressedScope(std::size_t line, std::size_t column) const {
  if ((!lc_view_.inhibit_pairs_in_strings && !lc_view_.inhibit_pairs_in_comments) ||
      line >= document_->lines.size()) {
    return false;
  }
  const std::string& text = document_->lines[line];
  if (text.empty()) {
    return false;
  }
  const auto& tokens = HighlightedLineTokens(line);
  if (tokens.empty()) {
    return false;
  }
  const std::size_t clamped_column = TextLayout::ClampTextColumn(text, column);
  const std::size_t token_index = clamped_column == 0 ? 0 : std::min(clamped_column - 1, tokens.size() - 1);
  const SyntaxTokenKind kind = tokens[token_index];
  if (lc_view_.inhibit_pairs_in_strings && kind == SyntaxTokenKind::String) {
    return true;
  }
  if (lc_view_.inhibit_pairs_in_comments && kind == SyntaxTokenKind::Comment) {
    return true;
  }
  return false;
}

bool TextViewport::TryMultiCaretPairInsert(char ch) {
  if (!has_multiple_carets()) {
    return false;
  }
  last_applied_edit_.reset();
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  struct Slot {
    bool is_primary = false;
    std::size_t secondary_index = 0;
    TextPosition reference{};
    std::optional<SelectionRange> selection;
  };

  auto slot_sort_end = [](const Slot& s) -> TextPosition {
    if (s.selection.has_value()) {
      return detail::RangeEndExclusive(*s.selection);
    }
    return s.reference;
  };

  std::vector<Slot> slots;
  slots.reserve(secondary_carets_.size() + 1);
  slots.push_back(Slot{
      .is_primary = true,
      .secondary_index = 0,
      .reference = {cursor_line_, cursor_column_},
      .selection = selection_range(),
  });
  for (std::size_t i = 0; i < secondary_carets_.size(); ++i) {
    const SecondaryCaret& sc = secondary_carets_[i];
    slots.push_back(Slot{
        .is_primary = false,
        .secondary_index = i,
        .reference = sc.position,
        .selection = detail::SelectionRangeForSecondaryCaret(sc.position, sc.selection_anchor),
    });
  }
  std::sort(slots.begin(), slots.end(),
            [&](const Slot& a, const Slot& b) {
              return detail::PositionLess(slot_sort_end(a), slot_sort_end(b));
            });

  std::size_t slice_min_line = std::numeric_limits<std::size_t>::max();
  std::size_t slice_max_line = 0;
  for (const Slot& slot : slots) {
    std::size_t lo = 0;
    std::size_t hi = 0;
    if (slot.selection.has_value()) {
      const SelectionRange norm = NormalizeRange(*slot.selection);
      lo = std::min(norm.start.line, norm.end.line);
      hi = std::max(norm.start.line, norm.end.line);
    } else {
      if (document_->lines.empty()) {
        continue;
      }
      lo = std::min(slot.reference.line, document_->lines.size() - 1);
      hi = lo;
    }
    slice_min_line = std::min(slice_min_line, lo);
    slice_max_line = std::max(slice_max_line, hi);
  }

  std::vector<std::string> before_lines;
  std::size_t before_lines_start = 0;
  if (slice_min_line != std::numeric_limits<std::size_t>::max()) {
    slice_max_line = std::min(slice_max_line, document_->lines.size() - 1);
    before_lines_start = slice_min_line;
    before_lines.reserve(slice_max_line - slice_min_line + 1);
    for (std::size_t i = slice_min_line; i <= slice_max_line; ++i) {
      before_lines.push_back(document_->lines[i]);
    }
  }
  const std::size_t before_document_line_count = document_->lines.size();
  const ViewState before_state = CaptureViewState();

  std::vector<SecondaryCaret> new_secondaries = secondary_carets_;
  TextPosition primary_after{cursor_line_, cursor_column_};
  std::optional<TextPosition> new_primary_anchor = selection_anchor_;

  bool text_changed = false;
  bool caret_changed = false;

  for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
    const Slot& slot = *it;
    const std::size_t line = std::min(slot.reference.line, document_->lines.size() - 1);
    const std::size_t column =
        TextLayout::ClampTextColumn(document_->lines[line], slot.reference.column);
    const std::string& current_line = document_->lines[line];

    if (slot.selection.has_value()) {
      const SelectionRange norm = NormalizeRange(*slot.selection);
      if (!detail::ValidateRangeColumns(document_->lines, norm)) {
        if (!slot.is_primary) {
          // leave new_secondaries unchanged for this index
        }
        continue;
      }

      const auto* sur_pair =
          lc_view_.surround_enabled ? FindSurroundOpener(lc_view_, ch) : nullptr;
      if (sur_pair != nullptr && !sur_pair->open.empty() && !sur_pair->close.empty() &&
          !InInsertionSuppressedScope(norm.start.line, norm.start.column)) {
        const std::string inner = detail::TextBetweenLines(document_->lines, norm);
        const std::string replacement = sur_pair->open + inner + sur_pair->close;
        const std::string first_prefix =
            document_->lines[norm.start.line].substr(0, norm.start.column);
        TextPosition inner_anchor{};
        TextPosition inner_cursor{};
        PostSurroundInnerSelection(*sur_pair, inner, norm.start.line, first_prefix,
                                   &inner_anchor, &inner_cursor);
        const std::optional<HistoryEntry> entry = BuildRangeHistoryEntry(norm, replacement);
        if (!entry.has_value()) {
          continue;
        }
        text_changed = true;
        ApplyHistoryEntry(*entry, true);
        if (slot.is_primary) {
          primary_after = inner_cursor;
          new_primary_anchor = inner_anchor;
        } else {
          new_secondaries[slot.secondary_index] = SecondaryCaret{
              .position = inner_cursor,
              .preferred_column = PreferredColumnForCaret(inner_cursor),
              .selection_anchor = inner_anchor,
          };
        }
        continue;
      }

      const std::optional<HistoryEntry> entry =
          BuildRangeHistoryEntry(norm, std::string(1, static_cast<char>(ch)));
      if (!entry.has_value()) {
        continue;
      }
      text_changed = true;
      ApplyHistoryEntry(*entry, true);
      const TextPosition pos{entry->after_state.cursor_line, entry->after_state.cursor_column};
      if (slot.is_primary) {
        primary_after = pos;
        new_primary_anchor.reset();
      } else {
        new_secondaries[slot.secondary_index] = SecondaryCaret{
            .position = pos,
            .preferred_column = PreferredColumnForCaret(pos),
            .selection_anchor = std::nullopt,
        };
      }
      continue;
    }

    const auto* close_pair = lc_view_.auto_close_enabled ? FindAutoCloseCloser(lc_view_, ch) : nullptr;
    if (close_pair != nullptr && column < current_line.size() && current_line[column] == ch) {
      const TextPosition updated_position{line, column + 1};
      caret_changed = true;
      if (slot.is_primary) {
        primary_after = updated_position;
        new_primary_anchor.reset();
      } else {
        new_secondaries[slot.secondary_index] = SecondaryCaret{
            .position = updated_position,
            .preferred_column = PreferredColumnForCaret(updated_position),
            .selection_anchor = std::nullopt,
        };
      }
      continue;
    }

    std::string replacement(1, ch);
    const auto* open_pair = lc_view_.auto_close_enabled ? FindAutoCloseOpener(lc_view_, ch) : nullptr;
    if (open_pair != nullptr && !open_pair->close.empty() && !InInsertionSuppressedScope(line, column) &&
        ShouldAutoCloseAtNext(current_line, column, lc_view_)) {
      if (!(open_pair->open == open_pair->close && column > 0 && current_line[column - 1] == ch)) {
        replacement = open_pair->open + open_pair->close;
      }
    }

    const std::optional<HistoryEntry> entry = BuildRangeHistoryEntry(
        SelectionRange{TextPosition{line, column}, TextPosition{line, column}}, replacement);
    if (!entry.has_value()) {
      continue;
    }
    text_changed = true;
    ApplyHistoryEntry(*entry, true);
    TextPosition updated_position{entry->after_state.cursor_line, entry->after_state.cursor_column};
    if (replacement.size() > 1) {
      updated_position.column = column + 1;
    }
    if (slot.is_primary) {
      primary_after = updated_position;
      new_primary_anchor.reset();
    } else {
      new_secondaries[slot.secondary_index] = SecondaryCaret{
          .position = updated_position,
          .preferred_column = PreferredColumnForCaret(updated_position),
          .selection_anchor = std::nullopt,
      };
    }
  }

  if (!text_changed && !caret_changed) {
    return false;
  }

  cursor_line_ = primary_after.line;
  cursor_column_ = primary_after.column;
  if (new_primary_anchor.has_value()) {
    selection_anchor_ = *new_primary_anchor;
  } else {
    selection_anchor_.reset();
  }
  secondary_carets_ = std::move(new_secondaries);
  std::sort(secondary_carets_.begin(), secondary_carets_.end(),
            [](const SecondaryCaret& lhs, const SecondaryCaret& rhs) {
              return detail::PositionLess(lhs.position, rhs.position);
            });
  DedupeSecondaryCaretsAgainstPrimary();
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  for (SecondaryCaret& sc : secondary_carets_) {
    sc.preferred_column = PreferredColumnForCaret(sc.position);
  }
  EnsureCursorVisible();

  if (!text_changed) {
    return true;
  }

  document_->placeholder = false;
  document_->dirty = true;
  const std::ptrdiff_t line_delta = static_cast<std::ptrdiff_t>(document_->lines.size()) -
                                    static_cast<std::ptrdiff_t>(before_document_line_count);
  const std::size_t after_slice_size =
      static_cast<std::size_t>(std::max<std::ptrdiff_t>(
          0, static_cast<std::ptrdiff_t>(before_lines.size()) + line_delta));
  const std::size_t after_slice_start = std::min(before_lines_start, document_->lines.size());
  const std::size_t after_slice_end =
      std::min(document_->lines.size(), before_lines_start + after_slice_size);
  std::vector<std::string> after_lines_slice;
  if (after_slice_start < after_slice_end) {
    after_lines_slice.assign(
        document_->lines.begin() + static_cast<std::ptrdiff_t>(after_slice_start),
        document_->lines.begin() + static_cast<std::ptrdiff_t>(after_slice_end));
  }
  HistoryEntry aggregate_entry = TextViewportUndoHistory::BuildEntryForDocumentChange(
      before_lines, before_state, after_lines_slice, CaptureViewState());
  aggregate_entry.start_line += before_lines_start;
  last_applied_edit_ = TextViewportUndoHistory::BuildAppliedEdit(aggregate_entry, true);
  PushHistoryEntry(aggregate_entry);
  return true;
}

}  // namespace microide::editor
