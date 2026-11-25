#include "editor/TextViewport.h"
#include "editor/TextViewportInternal.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/EditTypes.h"
#include "editor/TextLayout.h"
#include "util/StringUtil.h"

namespace microide::editor {

namespace {

void PostSurroundInnerSelection(const LanguagePair& pair,
                                std::string_view inner,
                                std::size_t start_line,
                                std::size_t first_line_prefix_size,
                                TextPosition* anchor,
                                TextPosition* cursor) {
  const util::LineShape inner_shape = util::MeasureLines(inner);
  *anchor = TextPosition{start_line, first_line_prefix_size + pair.open.size()};
  const std::size_t last_line = start_line + inner_shape.count - 1;
  const std::size_t single_line_offset =
      (last_line == start_line) ? first_line_prefix_size + pair.open.size() : 0;
  *cursor = TextPosition{last_line, single_line_offset + inner_shape.last_line_size};
}

bool IsIndentCharacter(char c) {
  return c == ' ' || c == '\t';
}

bool LineIsIndentOnly(std::string_view line) {
  return std::all_of(line.begin(), line.end(),
                     [](char c) { return IsIndentCharacter(c); });
}

std::string_view TrimmedRightView(std::string_view line) {
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

const LanguagePair* UsableSurroundPair(const LanguageContractView& view, char ch) {
  if (!view.surround_enabled) {
    return nullptr;
  }
  const LanguagePair* pair = FindSurroundOpener(view, ch);
  return pair != nullptr && !pair->open.empty() && !pair->close.empty() ? pair : nullptr;
}

bool ShouldAutoCloseAtNext(bool at_end, char next, const LanguageContractView& view) {
  if (at_end) {
    return true;
  }
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

bool QuoteAutoCloseSuppressed(const LanguagePair& pair, char ch, const CaretNeighborhood& at) {
  if (pair.open != pair.close || !at.has_prev) {
    return false;
  }
  if (at.prev == ch) {
    return true;
  }
  const bool quote_like = ch == '\'' || ch == '"' || ch == '`';
  return quote_like && (IsIdentifierByte(at.prev) || static_cast<unsigned char>(at.prev) >= 0x80);
}

bool DedentOnCloseTokenMatches(const LanguageContractView& view, char ch) {
  for (const auto& token : view.dedent_on_close_chars) {
    if (token.size() == 1 && token[0] == ch) {
      return true;
    }
  }
  return false;
}

}

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

  if (!language_contract_view().smart_indent_enabled) {
    return base_indent;
  }
  if (const std::optional<std::string> continued =
          DocCommentContinuation(current_line, clamped_column, base_indent);
      continued.has_value()) {
    return *continued;
  }
  if (language_contract_view().indent_after_open_patterns.empty()) {
    return base_indent;
  }

  const std::string_view trimmed = TrimmedRightView(current_line);
  if (clamped_column < trimmed.size()) {
    return base_indent;
  }
  for (const std::string& pattern : language_contract_view().indent_after_open_patterns) {
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

std::optional<std::string> TextViewport::DocCommentContinuation(std::string_view line,
                                                                std::size_t column,
                                                                const std::string& base_indent) const {
  if (language_contract_view().block_comment_open != "/*") {
    return std::nullopt;
  }
  const std::string_view before = line.substr(0, std::min(column, line.size()));
  const std::size_t text = before.find_first_not_of(" \t");
  if (text == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view body = before.substr(text);
  if (body.starts_with("/**") && !body.starts_with("/**/") &&
      body.find("*/", 3) == std::string_view::npos) {
    return base_indent + " * ";
  }
  if (body.starts_with("*") && !body.starts_with("*/") && (body.size() == 1 || body[1] == ' ') &&
      body.find("*/") == std::string_view::npos && text > 0 && before[text - 1] == ' ') {
    return base_indent + "* ";
  }
  return std::nullopt;
}

std::string TextViewport::IndentUnit() const {
  if (soft_tabs_) {
    return std::string(std::max<std::size_t>(1, indent_width_), ' ');
  }
  return "\t";
}

bool TextViewport::TryAutoCloseInsert(char ch) {
  if (!language_contract_view().auto_close_enabled || has_selection()) {
    return false;
  }
  const auto* pair = FindAutoCloseOpener(language_contract_view(), ch);
  if (pair == nullptr || pair->close.empty()) {
    return false;
  }
  if (cursor_line_ >= document_->lines.size()) {
    return false;
  }
  const CaretNeighborhood at = ReadCaretNeighborhood(cursor_line_, cursor_column_);
  const std::size_t clamped_column = at.clamped_column;
  if (InInsertionSuppressedScope(cursor_line_, clamped_column)) {
    return false;
  }
  if (!ShouldAutoCloseAtNext(at.at_end, at.next, language_contract_view())) {
    return false;
  }
  if (QuoteAutoCloseSuppressed(*pair, ch, at)) {
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
  cursor_column_ = clamped_column + pair->open.size();
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  EnsureCursorVisible();
  return true;
}

bool TextViewport::TrySurroundInsert(char ch) {
  const auto sel = selection_range();
  if (!sel.has_value()) {
    return false;
  }
  const LanguagePair* pair = UsableSurroundPair(language_contract_view(), ch);
  if (pair == nullptr) {
    return false;
  }
  const SelectionRange norm = NormalizeRange(*sel);
  if (!detail::ValidateRangeColumns(document_->lines, norm)) {
    return false;
  }

  const bool boundary_safe =
      pair->open.find('\n') == std::string::npos && pair->open.find('\r') == std::string::npos &&
      pair->close.find('\n') == std::string::npos && pair->close.find('\r') == std::string::npos;

  TextPosition inner_anchor{};
  TextPosition inner_cursor{};
  if (boundary_safe) {
    inner_anchor = TextPosition{norm.start.line, norm.start.column + pair->open.size()};
    const bool single_line = norm.start.line == norm.end.line;
    const std::size_t last_inner_line_size =
        single_line ? norm.end.column - norm.start.column : norm.end.column;
    const std::size_t single_line_offset = single_line ? norm.start.column + pair->open.size() : 0;
    inner_cursor = TextPosition{norm.end.line, single_line_offset + last_inner_line_size};
    if (!SurroundRangeBoundaries(norm, pair->open, pair->close)) {
      return false;
    }
  } else {
    const std::string inner = detail::TextBetweenLines(document_->lines, norm);
    const std::string replacement = pair->open + inner + pair->close;
    PostSurroundInnerSelection(*pair, inner, norm.start.line, norm.start.column, &inner_anchor,
                               &inner_cursor);
    if (!ApplyRangeEdit(norm, replacement, true)) {
      return false;
    }
  }
  selection_anchor_ = inner_anchor;
  cursor_line_ = inner_cursor.line;
  cursor_column_ = inner_cursor.column;
  preferred_column_ = PreferredColumnForCaret(inner_cursor);
  EnsureCursorVisible();
  return true;
}

bool TextViewport::TrySkipOverClose(char ch) {
  if (!language_contract_view().auto_close_enabled || has_selection()) {
    return false;
  }
  if (cursor_line_ >= document_->lines.size()) {
    return false;
  }
  const CaretNeighborhood at = ReadCaretNeighborhood(cursor_line_, cursor_column_);
  if (at.at_end || at.next != ch) {
    return false;
  }
  if (FindAutoCloseCloser(language_contract_view(), ch) == nullptr) {
    return false;
  }
  cursor_column_ = at.clamped_column + 1;
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  EnsureCursorVisible();
  return true;
}

bool TextViewport::MaybeDedentOnClose(char ch) {
  if (!language_contract_view().smart_indent_enabled) {
    return false;
  }
  if (has_selection()) {
    return false;
  }
  if (!DedentOnCloseTokenMatches(language_contract_view(), ch)) {
    return false;
  }
  if (cursor_line_ >= document_->lines.size()) {
    return false;
  }
  const std::size_t line_length = document_->lines.LineLength(cursor_line_);
  if (cursor_column_ != line_length) {
    return false;
  }
  const std::string unit = IndentUnit();
  if (line_length < unit.size()) {
    return false;
  }
  std::string scratch;
  if (document_->lines.LineWindow(cursor_line_, line_length - unit.size(), unit.size(), scratch) !=
      unit) {
    return false;
  }
  if (!LineIsIndentOnly(document_->lines.LineView(cursor_line_))) {
    return false;
  }
  const SelectionRange erase_range = SelectionRange{
      TextPosition{cursor_line_, line_length - unit.size()},
      TextPosition{cursor_line_, line_length},
  };
  return ApplyRangeEdit(erase_range, "", true);
}

std::optional<TextViewport::NewlineBraceSplit> TextViewport::ComputeNewlineBraceSplit(
    std::size_t line, std::size_t column) const {
  if (!language_contract_view().auto_close_enabled) {
    return std::nullopt;
  }
  if (line >= document_->lines.size()) {
    return std::nullopt;
  }
  if (!CaretSitsInsideAutoClosedPair(line, column)) {
    return std::nullopt;
  }
  const std::string base_indent = AutoIndentForNewline(line, column);
  const std::string inner_indent = base_indent + IndentUnit();
  return NewlineBraceSplit{
      .text = std::string("\n") + inner_indent + "\n" + base_indent,
      .inner_indent = inner_indent,
  };
}

bool TextViewport::CaretSitsInsideAutoClosedPair(std::size_t line, std::size_t column) const {
  if (!language_contract_view().auto_close_enabled || line >= document_->lines.size()) {
    return false;
  }
  const CaretNeighborhood at = ReadCaretNeighborhood(line, column);
  if (!at.has_prev || at.at_end) {
    return false;
  }
  const auto* opener = FindAutoCloseOpener(language_contract_view(), at.prev);
  return opener != nullptr && opener->close.size() == 1 && opener->close[0] == at.next;
}

bool TextViewport::TryInsertNewlineSplitBraces() {
  if (has_selection() || has_multiple_carets()) {
    return false;
  }
  if (cursor_line_ >= document_->lines.size()) {
    return false;
  }
  const std::size_t column =
      ReadCaretNeighborhood(cursor_line_, cursor_column_).clamped_column;
  const std::optional<NewlineBraceSplit> split = ComputeNewlineBraceSplit(cursor_line_, column);
  if (!split.has_value()) {
    return false;
  }
  const std::size_t opener_line = cursor_line_;
  const SelectionRange range = SelectionRange{
      TextPosition{cursor_line_, column},
      TextPosition{cursor_line_, column},
  };
  if (!ApplyRangeEdit(range, split->text, true)) {
    return false;
  }
  cursor_line_ = opener_line + 1;
  if (cursor_line_ < document_->lines.size()) {
    cursor_column_ =
        std::min(split->inner_indent.size(), document_->lines.LineLength(cursor_line_));
  } else {
    cursor_column_ = 0;
  }
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  EnsureCursorVisible();
  return true;
}

bool TextViewport::InInsertionSuppressedScope(std::size_t line, std::size_t column) const {
  if ((!language_contract_view().inhibit_pairs_in_strings && !language_contract_view().inhibit_pairs_in_comments) ||
      line >= document_->lines.size()) {
    return false;
  }
  if (document_->lines.LineLength(line) == 0) {
    return false;
  }
  const auto& tokens = HighlightedLineTokens(line);
  if (tokens.empty()) {
    return false;
  }
  const std::size_t clamped_column = ReadCaretNeighborhood(line, column).clamped_column;
  const std::size_t token_index = clamped_column == 0 ? 0 : std::min(clamped_column - 1, tokens.size() - 1);
  const SyntaxTokenKind kind = tokens[token_index];
  if (language_contract_view().inhibit_pairs_in_strings && kind == SyntaxTokenKind::String) {
    return true;
  }
  if (language_contract_view().inhibit_pairs_in_comments && kind == SyntaxTokenKind::Comment) {
    return true;
  }
  return false;
}

bool TextViewport::TryMultiCaretPairInsert(char ch) {
  if (!has_multiple_carets()) {
    return false;
  }
  if (MultiCaretSelectionsOverlap()) {
    return false;
  }
  ClearLastAppliedEdit();
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.PushBackLine("");
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

  detail::DisjointEditCapture capture(document_->lines);
  {
    std::vector<std::optional<detail::DisjointEditCapture::LineRange>> slot_lines(slots.size());
    const std::size_t last_line = document_->lines.size() - 1;
    for (std::size_t i = 0; i < slots.size(); ++i) {
      std::size_t lo = std::min(slots[i].reference.line, last_line);
      std::size_t hi = lo;
      if (slots[i].selection.has_value()) {
        const SelectionRange norm = NormalizeRange(*slots[i].selection);
        lo = std::min(norm.start.line, norm.end.line);
        hi = std::max(norm.start.line, norm.end.line);
      }
      slot_lines[i] = detail::DisjointEditCapture::LineRange{.first = lo, .last = std::min(hi, last_line)};
    }
    capture.PlanFootprints(slot_lines);
  }

  const ViewState before_state = CaptureViewState();
  const std::vector<SecondaryCaret> carets_before = secondary_carets_;
  std::optional<SuppressEntryCaretCapture> suppress_dead_caret_copies;
  suppress_dead_caret_copies.emplace(*this);

  std::vector<detail::MultiCaretRemapSite> sites(slots.size());

  bool text_changed = false;
  bool caret_changed = false;

  for (std::size_t slot_index = slots.size(); slot_index-- > 0;) {
    capture.EnterSite(slot_index);
    struct LeaveSiteGuard {
      detail::DisjointEditCapture& capture;
      std::size_t site;
      ~LeaveSiteGuard() { capture.LeaveSite(site); }
    } leave_guard{capture, slot_index};

    const Slot& slot = slots[slot_index];
    const std::size_t line = std::min(slot.reference.line, document_->lines.size() - 1);
    const CaretNeighborhood at = ReadCaretNeighborhood(line, slot.reference.column);
    const std::size_t column = at.clamped_column;

    if (slot.selection.has_value()) {
      const SelectionRange norm = NormalizeRange(*slot.selection);
      if (!detail::ValidateRangeColumns(document_->lines, norm)) {
        sites[slot_index] = detail::MultiCaretRemapSite{
            .landed = TextPosition{line, column},
            .anchor = slot.is_primary
                          ? before_state.selection_anchor
                          : carets_before[slot.secondary_index].selection_anchor};
        continue;
      }

      if (const LanguagePair* sur_pair = UsableSurroundPair(language_contract_view(), ch);
          sur_pair != nullptr) {
        const std::string inner = detail::TextBetweenLines(document_->lines, norm);
        const std::string replacement = sur_pair->open + inner + sur_pair->close;
        TextPosition inner_anchor{};
        TextPosition inner_cursor{};
        PostSurroundInnerSelection(*sur_pair, inner, norm.start.line, norm.start.column,
                                   &inner_anchor, &inner_cursor);
        const std::optional<HistoryEntry> entry = BuildRangeHistoryEntry(norm, replacement);
        if (!entry.has_value()) {
          sites[slot_index] =
              detail::MultiCaretRemapSite{.landed = TextPosition{line, column}};
          continue;
        }
        text_changed = true;
        ApplyHistoryEntry(*entry, true);
        sites[slot_index] =
            detail::MultiCaretRemapSite{.landed = inner_cursor,
                                        .anchor = inner_anchor,
                                        .has_edit = true,
                                        .removed = norm,
                                        .shape = detail::ComputeReplacementShape(replacement)};
        continue;
      }

      const std::string replacement(1, static_cast<char>(ch));
      const std::optional<HistoryEntry> entry = BuildRangeHistoryEntry(norm, replacement);
      if (!entry.has_value()) {
        caret_changed = true;
        sites[slot_index] = detail::MultiCaretRemapSite{
            .landed = TextPosition{norm.start.line, norm.start.column + replacement.size()}};
        continue;
      }
      text_changed = true;
      ApplyHistoryEntry(*entry, true);
      const TextPosition pos{entry->after_state.cursor_line, entry->after_state.cursor_column};
      sites[slot_index] =
          detail::MultiCaretRemapSite{.landed = pos,
                                      .has_edit = true,
                                      .removed = norm,
                                      .shape = detail::ComputeReplacementShape(replacement)};
      continue;
    }

    const auto* close_pair = language_contract_view().auto_close_enabled ? FindAutoCloseCloser(language_contract_view(), ch) : nullptr;
    if (close_pair != nullptr && !at.at_end && at.next == ch) {
      caret_changed = true;
      sites[slot_index] =
          detail::MultiCaretRemapSite{.landed = TextPosition{line, column + 1}};
      continue;
    }

    std::string replacement(1, ch);
    const auto* open_pair = language_contract_view().auto_close_enabled ? FindAutoCloseOpener(language_contract_view(), ch) : nullptr;
    if (open_pair != nullptr && !open_pair->close.empty() && !InInsertionSuppressedScope(line, column) &&
        ShouldAutoCloseAtNext(at.at_end, at.next, language_contract_view())) {
      if (!QuoteAutoCloseSuppressed(*open_pair, ch, at)) {
        replacement = open_pair->open + open_pair->close;
      }
    }

    const SelectionRange removed{TextPosition{line, column}, TextPosition{line, column}};
    const std::optional<HistoryEntry> entry = BuildRangeHistoryEntry(removed, replacement);
    if (!entry.has_value()) {
      sites[slot_index] = detail::MultiCaretRemapSite{.landed = TextPosition{line, column}};
      continue;
    }
    text_changed = true;
    ApplyHistoryEntry(*entry, true);
    TextPosition updated_position{entry->after_state.cursor_line, entry->after_state.cursor_column};
    if (replacement.size() > 1) {
      updated_position.column = column + 1;
    }
    sites[slot_index] =
        detail::MultiCaretRemapSite{.landed = updated_position,
                                    .has_edit = true,
                                    .removed = removed,
                                    .shape = detail::ComputeReplacementShape(replacement)};
  }

  suppress_dead_caret_copies.reset();
  if (!text_changed && !caret_changed) {
    return false;
  }

  detail::ResolveMultiCaretRemapSites(sites);

  TextPosition primary_after{cursor_line_, cursor_column_};
  std::optional<TextPosition> new_primary_anchor = selection_anchor_;
  std::vector<SecondaryCaret> new_secondaries = carets_before;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    const detail::MultiCaretRemapSite& site = sites[i];
    if (slots[i].is_primary) {
      primary_after = site.landed;
      new_primary_anchor = site.anchor;
    } else {
      new_secondaries[slots[i].secondary_index] = SecondaryCaret{
          .position = site.landed,
          .preferred_column = PreferredColumnForCaret(site.landed),
          .selection_anchor = site.anchor,
      };
    }
  }

  cursor_line_ = primary_after.line;
  cursor_column_ = primary_after.column;
  if (new_primary_anchor.has_value()) {
    selection_anchor_ = *new_primary_anchor;
  } else {
    selection_anchor_.reset();
  }
  secondary_carets_ = std::move(new_secondaries);
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
  HistoryEntry aggregate_entry = capture.Build(before_state, CaptureViewState());
  PushHistoryEntry(std::move(aggregate_entry),
                   MultiCaretCoalesceHint(MultiCaretEditKind::Insert, std::string_view(&ch, 1),
                                          false));
  return true;
}

}
