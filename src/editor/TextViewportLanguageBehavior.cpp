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

  // A whitespace-only line (first_non_indent == end) deliberately yields NO base
  // indent: pressing Enter on an auto-indented blank line clears the pending indent
  // instead of carrying it forward, so empty lines never accumulate trailing
  // whitespace. Pinned by TextViewport/InsertNewlineOnWhitespaceOnlyLineDoesNotCarryIndentForward.
  std::string base_indent;
  if (first_non_indent != current_line.end()) {
    const std::size_t indent_columns = std::min<std::size_t>(
        static_cast<std::size_t>(first_non_indent - current_line.begin()),
        clamped_column);
    base_indent.assign(current_line, 0, indent_columns);
  }

  if (!language_contract_view().smart_indent_enabled || language_contract_view().indent_after_open_patterns.empty()) {
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
  // Same-character pairs (quotes): avoid stacking when the previous char is
  // already the same quote (typing inside a comment/string we can't detect
  // semantically) -- simple heuristic.
  if (pair->open == pair->close && at.has_prev && at.prev == ch) {
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
  if (!language_contract_view().surround_enabled) {
    return false;
  }
  const auto sel = selection_range();
  if (!sel.has_value()) {
    return false;
  }
  const auto* pair = FindSurroundOpener(language_contract_view(), ch);
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

  // Surround only prepends `open` to the selection's first line and appends
  // `close` to its last line; the middle lines are untouched. Wrap the boundary
  // lines in place instead of materializing the whole selected text and the
  // open+inner+close transient the generic range-replace path builds (A-021).
  // Delimiters never carry a line break in practice (openers are single chars),
  // but guard and fall back to the generic path if one does so line accounting
  // stays correct.
  const bool boundary_safe =
      pair->open.find('\n') == std::string::npos && pair->open.find('\r') == std::string::npos &&
      pair->close.find('\n') == std::string::npos && pair->close.find('\r') == std::string::npos;

  TextPosition inner_anchor{};
  TextPosition inner_cursor{};
  if (boundary_safe) {
    // Inner-selection endpoints derived from the range, no text materialization:
    // the anchor sits just past `open` on the first line; the cursor sits at the
    // end of the selected content on the last line (offset by `open` too when the
    // selection is single-line, mirroring PostSurroundInnerSelection).
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
    const std::string first_prefix = document_->lines[norm.start.line].substr(0, norm.start.column);
    PostSurroundInnerSelection(*pair, inner, norm.start.line, first_prefix, &inner_anchor,
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
  // This asks one question about one byte; reading the whole line to answer it
  // cost megabytes per inserted character on a file with no line breaks in it.
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
  // Order matters for cost, not just for clarity: the caret-at-end-of-line and
  // long-enough-for-an-indent-unit tests read no text at all, and they exclude
  // every line this can act on except an indent-only one. Asking them first means
  // the line itself is only read when the caret sits at the end of a short line —
  // never on the file shape where reading it is megabytes (TD-2026-08-05-133).
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
  // Two bytes, read as two bytes: `column` is already a clamped code-point start
  // at every call site, so the neighbourhood read below re-clamps to itself.
  const CaretNeighborhood at = ReadCaretNeighborhood(line, column);
  if (!at.has_prev || at.at_end) {
    return std::nullopt;
  }
  const auto* opener = FindAutoCloseOpener(language_contract_view(), at.prev);
  if (opener == nullptr || opener->close.size() != 1 || opener->close[0] != at.next) {
    return std::nullopt;
  }
  const std::string base_indent = AutoIndentForNewline(line, column);
  const std::string inner_indent = base_indent + IndentUnit();
  return NewlineBraceSplit{
      .text = std::string("\n") + inner_indent + "\n" + base_indent,
      .inner_indent = inner_indent,
  };
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
  // Refuse if any two carets' selections overlap: this fast path (like the general
  // ApplyMultiCaretEdit) applies edits per caret and would double-edit shared
  // content. Returning false leaves the buffer untouched; the caller's fallback
  // (ApplyMultiCaretInsert) re-checks and also refuses, so nothing is mutated.
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
    // One walk of the piece tree for the whole range. The per-line `lines[i]` this
    // replaced went through TextBuffer::LineRef, which materialises a std::string
    // AND inserts it into the buffer's line cache — so capturing the span between
    // two far-apart carets cost two allocations per line and evicted the cache.
    before_lines = document_->lines.SliceLines(slice_min_line, slice_max_line + 1);
  }
  const std::size_t before_document_line_count = document_->lines.size();
  const ViewState before_state = CaptureViewState();

  // Record each slot's post-edit caret (and selection anchor) plus its edit
  // footprint, indexed by ascending slot order. Edits are applied high-to-low so
  // each stays valid against the still-unedited lower buffer; the lower-edit
  // remap is then batched in one pass (detail::ResolveMultiCaretRemapSites)
  // instead of remapping every already-recorded caret after each apply (which was
  // O(carets^2)). `landed` holds each caret's own post-edit position before the
  // lower-edit fold-in — exactly what the reverse walk recorded before remapping.
  std::vector<detail::MultiCaretRemapSite> sites(slots.size());

  bool text_changed = false;
  bool caret_changed = false;

  for (std::size_t slot_index = slots.size(); slot_index-- > 0;) {
    const Slot& slot = slots[slot_index];
    const std::size_t line = std::min(slot.reference.line, document_->lines.size() - 1);
    // Same bounded read as the single-caret pair paths: everything this loop asks
    // about the caret's own line is the byte on either side of it, and reading
    // the line to get them is unbounded on a document with no line breaks in it.
    const CaretNeighborhood at = ReadCaretNeighborhood(line, slot.reference.column);
    const std::size_t column = at.clamped_column;

    if (slot.selection.has_value()) {
      const SelectionRange norm = NormalizeRange(*slot.selection);
      if (!detail::ValidateRangeColumns(document_->lines, norm)) {
        // Range no longer valid: keep this caret in place (it still shifts if a
        // lower edit moves it).
        sites[slot_index] = detail::MultiCaretRemapSite{
            .landed = TextPosition{line, column},
            .anchor = slot.is_primary
                          ? selection_anchor_
                          : secondary_carets_[slot.secondary_index].selection_anchor};
        continue;
      }

      const auto* sur_pair =
          language_contract_view().surround_enabled ? FindSurroundOpener(language_contract_view(), ch) : nullptr;
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
        sites[slot_index] = detail::MultiCaretRemapSite{.landed = TextPosition{line, column}};
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
      if (!(open_pair->open == open_pair->close && at.has_prev && at.prev == ch)) {
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

  if (!text_changed && !caret_changed) {
    return false;
  }

  // Fold every lower edit into each recorded caret+anchor in one batched pass.
  detail::ResolveMultiCaretRemapSites(sites);

  TextPosition primary_after{cursor_line_, cursor_column_};
  std::optional<TextPosition> new_primary_anchor = selection_anchor_;
  std::vector<SecondaryCaret> new_secondaries = secondary_carets_;
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
  HistoryEntry aggregate_entry = TextViewportUndoHistory::BuildEntryForDocumentChange(
      std::move(before_lines), before_state,
      document_->lines.SliceLines(after_slice_start, after_slice_end), CaptureViewState());
  aggregate_entry.start_line += before_lines_start;
  // This path only runs with multiple carets (has_multiple_carets() gate above),
  // so the aggregate always spans disjoint regions. Publishing it as a single
  // contiguous AppliedEdit would drag markers on preserved interior lines to the
  // span's end (see TextViewportMultiCaret). Leave last_applied_edit_ empty (it was
  // reset at entry) so single-range marker consumers take their resync fallback.
  PushHistoryEntry(std::move(aggregate_entry));
  return true;
}

}  // namespace microide::editor
