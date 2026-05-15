#include "editor/TextViewport.h"

#include <algorithm>
#include <cctype>
#include <limits>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::editor {

namespace {

constexpr std::size_t kScrollMargin = 3;
constexpr std::size_t kHorizontalScrollMargin = 6;
constexpr std::size_t kMaxHistoryEntries = 128;
constexpr std::size_t kVisibleLineCacheLimit = 256;
constexpr std::size_t kHighlightCacheLimit = 256;
constexpr std::size_t kHighlightCheckpointInterval = 128;

bool IsCachedHighlightState(const SyntaxState& state) {
  return state.definition_id != 0;
}

std::string ToLower(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return lowered;
}

bool TextPositionLess(const TextPosition& lhs, const TextPosition& rhs) {
  if (lhs.line != rhs.line) {
    return lhs.line < rhs.line;
  }
  return lhs.column < rhs.column;
}

inline bool PositionLessTb(const TextPosition& lhs, const TextPosition& rhs) {
  if (lhs.line != rhs.line) {
    return lhs.line < rhs.line;
  }
  return lhs.column < rhs.column;
}

std::optional<SelectionRange> SelectionRangeForSecondaryCaret(
    const TextPosition& position,
    const std::optional<TextPosition>& selection_anchor) {
  if (!selection_anchor.has_value()) {
    return std::nullopt;
  }
  if (selection_anchor->line == position.line && selection_anchor->column == position.column) {
    return std::nullopt;
  }
  if (PositionLessTb(*selection_anchor, position)) {
    return SelectionRange{*selection_anchor, position};
  }
  return SelectionRange{position, *selection_anchor};
}

TextPosition RangeEndExclusive(const SelectionRange& r) {
  return PositionLessTb(r.start, r.end) ? r.end : r.start;
}

bool ValidateRangeColumns(const std::vector<std::string>& lines, const SelectionRange& n) {
  if (n.start.line >= lines.size() || n.end.line >= lines.size()) {
    return false;
  }
  if (n.start.column > lines[n.start.line].size() || n.end.column > lines[n.end.line].size()) {
    return false;
  }
  return true;
}

std::string TextBetweenLines(const std::vector<std::string>& lines, const SelectionRange& n) {
  const auto& a = n.start;
  const auto& b = n.end;
  if (a.line == b.line) {
    return lines[a.line].substr(a.column, b.column - a.column);
  }
  std::string out;
  out += lines[a.line].substr(a.column);
  out.push_back('\n');
  for (std::size_t i = a.line + 1; i < b.line; ++i) {
    out += lines[i];
    out.push_back('\n');
  }
  out += lines[b.line].substr(0, b.column);
  return out;
}

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

const microide::editor::LanguagePair* FindAutoCloseOpener(
    const microide::editor::LanguageContractView& view, char ch) {
  for (const auto& pair : view.auto_close_pairs) {
    if (pair.open.size() == 1 && pair.open[0] == ch) {
      return &pair;
    }
  }
  return nullptr;
}

const microide::editor::LanguagePair* FindAutoCloseCloser(
    const microide::editor::LanguageContractView& view, char ch) {
  for (const auto& pair : view.auto_close_pairs) {
    if (pair.close.size() == 1 && pair.close[0] == ch) {
      return &pair;
    }
  }
  return nullptr;
}

const microide::editor::LanguagePair* FindSurroundOpener(
    const microide::editor::LanguageContractView& view, char ch) {
  for (const auto& pair : view.surround_pairs) {
    if (pair.open.size() == 1 && pair.open[0] == ch) {
      return &pair;
    }
  }
  return nullptr;
}

bool ShouldAutoCloseAtNext(const std::string& line, std::size_t column,
                           const microide::editor::LanguageContractView& view) {
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

bool DedentOnCloseTokenMatches(const microide::editor::LanguageContractView& view,
                               char ch) {
  for (const auto& token : view.dedent_on_close_chars) {
    if (token.size() == 1 && token[0] == ch) {
      return true;
    }
  }
  return false;
}

bool TrimTrailingWhitespaceInPlace(std::vector<std::string>& lines) {
  bool any = false;
  for (std::string& line : lines) {
    std::size_t end = line.size();
    while (end > 0) {
      char c = line[end - 1];
      if (c != ' ' && c != '\t') break;
      --end;
    }
    if (end != line.size()) {
      line.resize(end);
      any = true;
    }
  }
  return any;
}

bool EnsureSingleFinalNewlineInPlace(std::vector<std::string>& lines) {
  if (lines.empty()) {
    lines.emplace_back();
    return true;
  }
  bool changed = false;
  while (lines.size() > 1 && lines.back().empty() && lines[lines.size() - 2].empty()) {
    lines.pop_back();
    changed = true;
  }
  if (lines.empty() || !lines.back().empty()) {
    lines.emplace_back();
    changed = true;
  }
  return changed;
}

}  // namespace

TextViewport::TextViewport() {
  document_ = std::make_shared<DocumentState>();
  SetPlaceholderText(
      "microide\n\n"
      "SDL3 shell scaffold is running.\n"
      "Open files from the sidebar with Enter.\n"
      "F8 toggles the sidebar, F6 toggles the overlay.\n");
}

bool TextViewport::OpenFile(const std::filesystem::path& path) {
  std::string perf_label = "TextViewport::OpenFile";
  if (util::PerformanceTrace::Enabled()) {
    perf_label += "(path=" + path.string() + ")";
  }
  util::PerformanceTrace::Scope perf_scope(perf_label);
  EnsureDocument();
  const std::optional<std::string> content = util::ReadTextFile(path);
  if (!content.has_value()) {
    return false;
  }

  const util::DecodedText decoded = util::DecodeLines(*content);
  ResetState(decoded.lines, path, decoded.line_ending, decoded.mixed_line_endings,
             DetectEncoding(*content), false, false);
  return true;
}

bool TextViewport::Save() {
  EnsureDocument();
  if (document_->path.empty()) {
    return false;
  }

  // Save-time normalization: apply trim and final-newline transforms to a
  // local copy of the lines buffer. We write the normalized text but keep the
  // in-memory buffer untouched unless the toggles change the content; this
  // keeps subsequent edits idempotent and avoids the noise of a re-layout
  // immediately after save.
  std::vector<std::string> normalized = document_->lines;
  bool changed = false;
  if (save_trim_trailing_whitespace_) {
    if (TrimTrailingWhitespaceInPlace(normalized)) changed = true;
  }
  if (save_ensure_final_newline_) {
    if (EnsureSingleFinalNewlineInPlace(normalized)) changed = true;
  }

  const std::string text = util::SerializeLines(
      changed ? normalized : document_->lines, document_->line_ending);
  if (!util::WriteTextFileAtomically(document_->path, text)) {
    return false;
  }

  if (changed) {
    // Mirror the normalization into the live buffer so the user sees the
    // same content they just saved. Routed through ReplaceLines so undo can
    // unwind the change.
    ReplaceLines(0, document_->lines.size(), normalized, /*record_undo=*/true);
  }

  document_->mixed_line_endings = false;
  document_->dirty = false;
  return true;
}

void TextViewport::LoadContent(std::string_view content,
                               const std::filesystem::path& path,
                               std::optional<LineEnding> line_ending) {
  EnsureDocument();
  const util::DecodedText decoded = util::DecodeLines(content);
  ResetState(decoded.lines, path, line_ending.value_or(decoded.line_ending),
             line_ending.has_value() ? false : decoded.mixed_line_endings, DetectEncoding(content),
             false, false);
}

void TextViewport::SetPath(const std::filesystem::path& path) {
  EnsureDocument();
  document_->path = path;
}

void TextViewport::SetDirty(bool dirty) {
  EnsureDocument();
  document_->dirty = dirty;
  if (dirty) {
    document_->placeholder = false;
  }
}

void TextViewport::SetPlaceholderText(std::string text) {
  EnsureDocument();
  ResetState(util::SplitLines(text), {}, LineEnding::LF, false, DetectEncoding(text), true, false);
}

void TextViewport::SetUntitledBuffer() {
  EnsureDocument();
  ResetState({""}, {}, LineEnding::LF, false, TextEncoding::ASCII, false, false);
}

void TextViewport::SetViewportSize(std::size_t visible_lines, std::size_t visible_columns) {
  const std::size_t next_visible_lines = std::max<std::size_t>(1, visible_lines);
  const std::size_t next_visible_columns = std::max<std::size_t>(8, visible_columns);
  const bool wrap_width_changed = soft_wrap_ && visible_columns_ != next_visible_columns;
  visible_lines_ = next_visible_lines;
  visible_columns_ = next_visible_columns;
  if (wrap_width_changed) {
    horizontal_scroll_ = 0;
    return;
  }
  ClampScrollState();
}

void TextViewport::SetScrollLine(std::size_t scroll_line) {
  scroll_line_ = scroll_line;
  ClampScrollState();
}

void TextViewport::SetHorizontalScroll(std::size_t horizontal_scroll) {
  horizontal_scroll_ = horizontal_scroll;
  ClampScrollState();
}

void TextViewport::SetTabSize(std::size_t tab_size) {
  const std::size_t next_tab_size = std::clamp<std::size_t>(tab_size, 1, 16);
  if (tab_size_ == next_tab_size) {
    return;
  }
  tab_size_ = next_tab_size;
  cached_max_visual_columns_.reset();
  cached_max_visual_columns_tab_size_ = 0;
  cached_max_visual_columns_revision_ = 0;
  visible_line_cache_.clear();
  visible_line_cache_order_.clear();
  ClampCursorColumn();
  ClampScrollState();
  EnsureCursorVisible();
}

void TextViewport::SetIndentWidth(std::size_t indent_width) {
  const std::size_t next_indent_width = std::clamp<std::size_t>(indent_width, 1, 16);
  if (indent_width_ == next_indent_width) {
    return;
  }
  indent_width_ = next_indent_width;
}

void TextViewport::SetSoftTabs(bool soft_tabs) {
  if (soft_tabs_ == soft_tabs) {
    return;
  }
  soft_tabs_ = soft_tabs;
}

void TextViewport::SetSoftWrap(bool soft_wrap) {
  if (soft_wrap_ == soft_wrap) {
    return;
  }
  soft_wrap_ = soft_wrap;
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  for (SecondaryCaret& caret : secondary_carets_) {
    caret.preferred_column = PreferredColumnForCaret(caret.position);
  }
  ClampScrollState();
  EnsureCursorVisible();
}

void TextViewport::SetFoldingModel(const FoldingModel* folding_model) {
  if (folding_model_ == folding_model) {
    return;
  }
  folding_model_ = folding_model;
  InvalidateVisualColumnCache();
  ClampScrollState();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorVertical(int delta, bool extend_selection) {
  if (document_->lines.empty() || delta == 0) {
    return;
  }

  BeginSelectionIfNeeded(extend_selection);
  TextPosition primary{cursor_line_, cursor_column_};
  AdvanceCaretVertical(primary, preferred_column_, delta);
  cursor_line_ = primary.line;
  cursor_column_ = primary.column;

  for (SecondaryCaret& caret : secondary_carets_) {
    AdvanceCaretVertical(caret.position, caret.preferred_column, delta);
  }
  DedupeSecondaryCaretsAgainstPrimary();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorHorizontal(int delta, bool extend_selection) {
  if (document_->lines.empty() || delta == 0) {
    return;
  }

  BeginSelectionIfNeeded(extend_selection);
  TextPosition primary{cursor_line_, cursor_column_};
  AdvanceCaretHorizontal(primary, delta);
  cursor_line_ = primary.line;
  cursor_column_ = primary.column;
  preferred_column_ = PreferredColumnForCaret(primary);

  for (SecondaryCaret& caret : secondary_carets_) {
    AdvanceCaretHorizontal(caret.position, delta);
    caret.preferred_column = PreferredColumnForCaret(caret.position);
  }
  DedupeSecondaryCaretsAgainstPrimary();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorLineStart(bool extend_selection) {
  BeginSelectionIfNeeded(extend_selection);
  cursor_column_ = 0;
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  for (SecondaryCaret& caret : secondary_carets_) {
    caret.position.column = 0;
    caret.preferred_column = PreferredColumnForCaret(caret.position);
  }
  DedupeSecondaryCaretsAgainstPrimary();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorLineEnd(bool extend_selection) {
  BeginSelectionIfNeeded(extend_selection);
  cursor_column_ = CurrentLineLength();
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  for (SecondaryCaret& caret : secondary_carets_) {
    if (caret.position.line < document_->lines.size()) {
      caret.position.column = document_->lines[caret.position.line].size();
    }
    caret.preferred_column = PreferredColumnForCaret(caret.position);
  }
  DedupeSecondaryCaretsAgainstPrimary();
  EnsureCursorVisible();
}

void TextViewport::MoveCursorTo(std::size_t line, std::size_t column, bool extend_selection) {
  if (document_->lines.empty()) {
    return;
  }

  BeginSelectionIfNeeded(extend_selection);
  cursor_line_ = std::min(line, document_->lines.size() - 1);
  cursor_column_ =
      TextLayout::ClampTextColumn(document_->lines[cursor_line_], std::min(column, CurrentLineLength()));
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  for (SecondaryCaret& caret : secondary_carets_) {
    caret.preferred_column = PreferredColumnForCaret(caret.position);
  }
  EnsureCursorVisible();
}

void TextViewport::MoveCursorToVisualColumn(std::size_t line,
                                            std::size_t visual_column,
                                            bool extend_selection) {
  if (document_->lines.empty()) {
    return;
  }

  const std::size_t clamped_line = std::min(line, document_->lines.size() - 1);
  const std::size_t text_column =
      TextLayout::TextColumnForVisualColumn(document_->lines[clamped_line], visual_column,
                                            tab_size_);
  MoveCursorTo(clamped_line, text_column, extend_selection);
}

void TextViewport::ScrollVertical(int delta) {
  if (document_->lines.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(scroll_line_);
  const int visual_rows = VisualRowCount();
  const int max_index = std::max(0, visual_rows - static_cast<int>(visible_lines_));
  scroll_line_ = static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
}

void TextViewport::Page(int direction) {
  if (direction == 0) {
    return;
  }
  const std::size_t step = visible_lines_ > 1 ? visible_lines_ - 1 : 1;
  MoveCursorVertical(static_cast<int>(step) * direction);
}

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
  (void)ApplyRangeEdit(range, text, true);
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
  const std::string newline_text =
      "\n" + AutoIndentForNewline(cursor_line_, cursor_column_);
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
    const std::size_t erase_start =
        TextLayout::PreviousTextColumn(document_->lines[cursor_line_], cursor_column_);
    (void)ApplyRangeEdit(
        SelectionRange{
            .start = TextPosition{cursor_line_, erase_start},
            .end = TextPosition{cursor_line_, cursor_column_},
        },
        "", true);
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
    (void)ApplyRangeEdit(
        SelectionRange{
            .start = TextPosition{cursor_line_, cursor_column_},
            .end = TextPosition{cursor_line_, erase_end},
        },
        "", true);
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
  if (!undo_group_stack_.empty()) {
    FlushActiveUndoGroup();
  }
  if (document_->undo_stack.empty()) {
    return false;
  }

  HistoryEntry entry = std::move(document_->undo_stack.back());
  document_->undo_stack.pop_back();
  entry.after_state = CaptureViewState();
  {
    util::PerformanceTrace::Scope scope("TextViewport::Undo::ApplyHistoryEntry");
    ApplyHistoryEntry(entry, false);
  }
  {
    util::PerformanceTrace::Scope scope("TextViewport::Undo::BuildAppliedEdit");
    last_applied_edit_ = BuildAppliedEditForHistoryEntry(entry, false);
  }
  document_->redo_stack.push_back(std::move(entry));
  return true;
}

bool TextViewport::Redo() {
  util::PerformanceTrace::Scope perf_scope("TextViewport::Redo");
  if (!undo_group_stack_.empty()) {
    FlushActiveUndoGroup();
  }
  if (document_->redo_stack.empty()) {
    return false;
  }

  HistoryEntry entry = std::move(document_->redo_stack.back());
  document_->redo_stack.pop_back();
  entry.before_state = CaptureViewState();
  {
    util::PerformanceTrace::Scope scope("TextViewport::Redo::ApplyHistoryEntry");
    ApplyHistoryEntry(entry, true);
  }
  {
    util::PerformanceTrace::Scope scope("TextViewport::Redo::BuildAppliedEdit");
    last_applied_edit_ = BuildAppliedEditForHistoryEntry(entry, true);
  }
  document_->undo_stack.push_back(std::move(entry));
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
  const std::string lowered_needle = ToLower(needle);
  const std::string lowered_replacement = ToLower(replacement);
  std::size_t replacements = 0;
  std::size_t first_changed_line = document_->lines.size();
  std::size_t last_changed_line = 0;
  std::vector<std::string> before_changed_lines;

  // Build the final document state in one pass per line, bypassing ApplyRangeEdit
  // so that InvalidateDerivedCaches / RefreshEncoding are called once at the end
  // rather than once per replacement.
  std::string new_line;
  for (std::size_t line_index = 0; line_index < document_->lines.size(); ++line_index) {
    std::string& current_line = document_->lines[line_index];
    std::string lowered_line = ToLower(current_line);
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
      lowered_line.replace(offset, needle.size(), lowered_replacement);
      offset = lowered_line.find(lowered_needle, offset + replacement.size());
    }
    new_line.append(current_line, copy_from);
    if (first_changed_line == document_->lines.size()) {
      first_changed_line = line_index;
    } else if (line_index > last_changed_line + 1) {
      for (std::size_t gap = last_changed_line + 1; gap < line_index; ++gap) {
        before_changed_lines.push_back(document_->lines[gap]);
      }
    }
    before_changed_lines.push_back(current_line);
    last_changed_line = line_index;
    current_line = std::move(new_line);
  }

  if (replacements > 0) {
    document_->dirty = true;
    document_->redo_stack.clear();
    RefreshEncoding();
    InvalidateLayoutCaches();
    EnsureCursorVisible();
    const ViewState after_state = CaptureViewState();
    PushHistoryEntry(HistoryEntry{
        .start_line = first_changed_line,
        .before_lines = std::move(before_changed_lines),
        .after_lines = SliceLines(document_->lines, first_changed_line, last_changed_line + 1),
        .before_state = before_state,
        .after_state = after_state,
    });
  }
  return replacements;
}

std::size_t TextViewport::cursor_visual_column() const {
  if (document_->lines.empty() || cursor_line_ >= document_->lines.size()) {
    return 0;
  }
  return TextLayout::VisualColumnForTextColumn(document_->lines[cursor_line_], cursor_column_,
                                               tab_size_);
}

std::string TextViewport::LineEndingLabel() const {
  const std::string base = util::LineEndingLabel(document_->line_ending);
  const std::string upper =
      base == "crlf" ? "CRLF" : base == "cr" ? "CR" : "LF";
  return document_->mixed_line_endings ? "mixed:" + upper : upper;
}

std::string TextViewport::EncodingLabel() const {
  switch (document_->encoding) {
    case TextEncoding::ASCII:
      return "ASCII";
    case TextEncoding::UTF8:
      return "UTF-8";
    case TextEncoding::Bytes:
    default:
      return "Bytes";
  }
}

LayoutLine TextViewport::VisibleLineLayout(std::size_t line_index) const {
  if (line_index >= document_->lines.size()) {
    return LayoutLine{};
  }

  ++visible_line_queries_;
  const VisibleLineCacheKey cache_key{
      .line_index = line_index,
      .horizontal_scroll = horizontal_scroll_,
      .visible_columns = visible_columns_,
      .tab_size = tab_size_,
  };
  LayoutLine layout;
  if (const auto it = visible_line_cache_.find(cache_key); it != visible_line_cache_.end()) {
    ++visible_line_hits_;
    layout = it->second;
  } else {
    layout = TextLayout::BuildVisibleLine(document_->lines[line_index], horizontal_scroll_,
                                          visible_columns_, tab_size_);
    if (visible_line_cache_.size() >= kVisibleLineCacheLimit) {
      visible_line_cache_.erase(visible_line_cache_order_.front());
      visible_line_cache_order_.pop_front();
    }
    visible_line_cache_.emplace(cache_key, layout);
    visible_line_cache_order_.push_back(cache_key);
  }

  if (line_index == cursor_line_) {
    const std::size_t caret_visual = TextLayout::VisualColumnForTextColumn(
        document_->lines[line_index], cursor_column_, tab_size_);
    if (caret_visual >= horizontal_scroll_ &&
        caret_visual <= horizontal_scroll_ + visible_columns_) {
      layout.caret_visible = true;
      layout.caret_column = caret_visual - horizontal_scroll_;
    } else {
      layout.caret_visible = false;
      layout.caret_column = 0;
    }
  } else {
    layout.caret_visible = false;
    layout.caret_column = 0;
  }
  return layout;
}

TextViewport::WrappedRowLayout TextViewport::WrappedRowAt(std::size_t visual_row_index) const {
  if (wrapped_row_layouts_trivial_) {
    return WrappedRowLayout{visual_row_index, horizontal_scroll_,
                            horizontal_scroll_ + visible_columns_};
  }
  if (wrapped_row_layouts_.empty()) {
    return WrappedRowLayout{};
  }
  const std::size_t clamped = std::min<std::size_t>(visual_row_index,
                                                    wrapped_row_layouts_.size() - 1);
  return wrapped_row_layouts_[clamped];
}

std::size_t TextViewport::WrappedRowCount() const {
  if (wrapped_row_layouts_trivial_) {
    return document_ != nullptr ? document_->lines.size() : 0;
  }
  return wrapped_row_layouts_.size();
}

std::size_t TextViewport::WrappedLineRowOffset(std::size_t line_index) const {
  if (wrapped_row_layouts_trivial_) {
    return line_index;
  }
  if (wrapped_line_row_offsets_.empty() || line_index >= wrapped_line_row_offsets_.size()) {
    return 0;
  }
  return wrapped_line_row_offsets_[line_index];
}

LayoutLine TextViewport::VisibleWrappedRowLayout(std::size_t visual_row_index) const {
  if (!soft_wrap_) {
    return VisibleLineLayout(visual_row_index);
  }
  EnsureWrappedRowLayouts();
  if (visual_row_index >= WrappedRowCount()) {
    return LayoutLine{};
  }

  const WrappedRowLayout row = WrappedRowAt(visual_row_index);
  LayoutLine layout = TextLayout::BuildVisibleLine(document_->lines[row.line_index], row.visual_start,
                                                   visible_columns_, tab_size_);
  if (row.line_index == cursor_line_ && visual_row_index == CursorVisualRow()) {
    const std::size_t caret_visual = cursor_visual_column();
    layout.caret_visible = true;
    layout.caret_column = caret_visual >= row.visual_start ? caret_visual - row.visual_start : 0;
  } else {
    layout.caret_visible = false;
    layout.caret_column = 0;
  }
  return layout;
}

TextViewport::WrappedVisualRow TextViewport::WrappedVisualRowLayout(std::size_t visual_row_index) const {
  EnsureWrappedRowLayouts();
  if (visual_row_index >= WrappedRowCount()) {
    return {};
  }
  const WrappedRowLayout row = WrappedRowAt(visual_row_index);
  return WrappedVisualRow{
      .line_index = row.line_index,
      .visual_start = row.visual_start,
      .visual_end = row.visual_end,
  };
}

LogicalPosition TextViewport::LogicalPositionForVisualHit(int visual_row, int visual_col) const {
  if (document_->lines.empty()) {
    return {};
  }
  EnsureWrappedRowLayouts();
  const std::size_t row_count = WrappedRowCount();
  if (row_count == 0) {
    return {};
  }
  const std::size_t clamped_row =
      std::min<std::size_t>(std::max(0, visual_row), row_count - 1);
  const WrappedRowLayout layout = WrappedRowAt(clamped_row);
  const std::size_t width = layout.visual_end - layout.visual_start;
  const std::size_t local_max = width;
  const std::size_t clamped_local = std::min<std::size_t>(std::max(0, visual_col), local_max);
  const std::size_t target_visual = layout.visual_start + clamped_local;
  return LogicalPosition{
      .line = layout.line_index,
      .column = TextLayout::TextColumnForVisualColumn(document_->lines[layout.line_index],
                                                       target_visual, tab_size_),
  };
}

int TextViewport::VisualRowCount() const {
  EnsureWrappedRowLayouts();
  return static_cast<int>(WrappedRowCount());
}

std::size_t TextViewport::visual_line_count() const {
  return static_cast<std::size_t>(std::max(0, VisualRowCount()));
}

std::size_t TextViewport::VisualRowLineIndex(std::size_t visual_row_index) const {
  EnsureWrappedRowLayouts();
  if (WrappedRowCount() == 0) {
    return 0;
  }
  return WrappedRowAt(visual_row_index).line_index;
}

std::size_t TextViewport::VisualRowForLine(std::size_t line_index) const {
  EnsureWrappedRowLayouts();
  if (wrapped_row_layouts_trivial_) {
    if (document_ == nullptr || document_->lines.empty()) {
      return 0;
    }
    return std::min<std::size_t>(line_index, document_->lines.size() - 1);
  }
  if (wrapped_line_row_offsets_.empty()) {
    return 0;
  }
  return wrapped_line_row_offsets_[std::min<std::size_t>(line_index, wrapped_line_row_offsets_.size() - 1)];
}

const std::vector<SyntaxTokenKind>& TextViewport::HighlightedLineTokens(
    std::size_t line_index) const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::HighlightedLineTokens");
  static const std::vector<SyntaxTokenKind> kEmptyTokens;
  if (line_index >= document_->lines.size()) {
    return kEmptyTokens;
  }
  if (!syntax_highlighting_enabled()) {
    return kEmptyTokens;
  }

  ++highlight_queries_;
  EnsureHighlightCaches();

  if (const auto it = highlight_cache_.find(line_index); it != highlight_cache_.end()) {
    util::PerformanceTrace::Scope hit_scope("TextViewport::HighlightedLineTokens::CacheHit");
    ++highlight_hits_;
    return it->second;
  }

  util::PerformanceTrace::Scope miss_scope("TextViewport::HighlightedLineTokens::CacheMiss");
  const SyntaxState previous_state = HighlightStateBeforeLine(line_index);
  HighlightedLine highlighted;
  {
    util::PerformanceTrace::Scope highlight_scope(
        "TextViewport::HighlightedLineTokens::HighlightLine");
    highlighted = SyntaxHighlighter::HighlightLine(document_->lines[line_index], document_->path,
                                                   previous_state);
  }
  line_highlight_states_[line_index] = highlighted.end_state;

  if (highlight_cache_.size() >= kHighlightCacheLimit) {
    highlight_cache_.erase(highlight_cache_order_.front());
    highlight_cache_order_.pop_front();
  }
  auto [it, _] = highlight_cache_.emplace(line_index, highlighted.tokens);
  highlight_cache_order_.push_back(line_index);
  return it->second;
}

TextViewportCacheStats TextViewport::CacheStats() const {
  return TextViewportCacheStats{
      .visible_line_queries = visible_line_queries_,
      .visible_line_hits = visible_line_hits_,
      .highlight_queries = highlight_queries_,
      .highlight_hits = highlight_hits_,
      .highlight_state_advances = highlight_state_advances_,
      .highlight_checkpoint_advances = highlight_checkpoint_advances_,
  };
}

void TextViewport::ResetCacheStats() const {
  visible_line_queries_ = 0;
  visible_line_hits_ = 0;
  highlight_queries_ = 0;
  highlight_hits_ = 0;
  highlight_state_advances_ = 0;
  highlight_checkpoint_advances_ = 0;
}

std::vector<TextPosition> TextViewport::secondary_carets() const {
  std::vector<TextPosition> carets;
  carets.reserve(secondary_carets_.size());
  for (const SecondaryCaret& caret : secondary_carets_) {
    carets.push_back(caret.position);
  }
  return carets;
}

void TextViewport::AddSecondaryCaret(std::size_t line, std::size_t column) {
  if (document_->lines.empty()) {
    return;
  }
  const std::size_t clamped_line = std::min(line, document_->lines.size() - 1);
  const std::size_t clamped_column =
      TextLayout::ClampTextColumn(document_->lines[clamped_line], column);
  const TextPosition position{clamped_line, clamped_column};
  if (position == TextPosition{cursor_line_, cursor_column_}) {
    return;
  }
  if (std::find_if(secondary_carets_.begin(), secondary_carets_.end(),
                   [&](const SecondaryCaret& caret) { return caret.position == position; }) !=
      secondary_carets_.end()) {
    return;
  }
  secondary_carets_.push_back(SecondaryCaret{
      .position = position,
      .preferred_column = PreferredColumnForCaret(position),
      .selection_anchor = std::nullopt,
  });
  std::sort(secondary_carets_.begin(), secondary_carets_.end(),
            [](const SecondaryCaret& lhs, const SecondaryCaret& rhs) {
              return TextPositionLess(lhs.position, rhs.position);
            });
}

void TextViewport::AddSecondaryCaretWithRange(SelectionRange range) {
  if (document_->lines.empty()) {
    return;
  }
  const SelectionRange norm = NormalizeRange(range);
  if (!ValidateRangeColumns(document_->lines, norm)) {
    return;
  }
  if (norm.start.line == norm.end.line && norm.start.column == norm.end.column) {
    AddSecondaryCaret(norm.start.line, norm.start.column);
    return;
  }
  TextPosition anchor = norm.start;
  TextPosition cursor_end = norm.end;
  if (!PositionLessTb(anchor, cursor_end)) {
    std::swap(anchor, cursor_end);
  }
  const std::size_t clamped_line = std::min(cursor_end.line, document_->lines.size() - 1);
  cursor_end.column = TextLayout::ClampTextColumn(document_->lines[clamped_line], cursor_end.column);
  anchor.line = std::min(anchor.line, document_->lines.size() - 1);
  anchor.column = TextLayout::ClampTextColumn(document_->lines[anchor.line], anchor.column);

  if (cursor_end == TextPosition{cursor_line_, cursor_column_}) {
    return;
  }
  const SecondaryCaret candidate{.position = cursor_end,
                                 .preferred_column = PreferredColumnForCaret(cursor_end),
                                 .selection_anchor = anchor};
  if (std::find_if(secondary_carets_.begin(), secondary_carets_.end(),
                   [&](const SecondaryCaret& caret) {
                     return caret.position == candidate.position &&
                            caret.selection_anchor == candidate.selection_anchor;
                   }) != secondary_carets_.end()) {
    return;
  }
  secondary_carets_.push_back(candidate);
  std::sort(secondary_carets_.begin(), secondary_carets_.end(),
            [](const SecondaryCaret& lhs, const SecondaryCaret& rhs) {
              return TextPositionLess(lhs.position, rhs.position);
            });
  DedupeSecondaryCaretsAgainstPrimary();
}

void TextViewport::SetSecondaryCarets(std::vector<TextPosition> carets) {
  secondary_carets_.clear();
  for (const TextPosition& caret : carets) {
    AddSecondaryCaret(caret.line, caret.column);
  }
}

void TextViewport::ClearSecondaryCarets() {
  secondary_carets_.clear();
}

bool TextViewport::has_selection() const {
  return selection_range().has_value();
}

std::optional<SelectionRange> TextViewport::selection_range() const {
  if (!selection_anchor_.has_value()) {
    return std::nullopt;
  }

  const TextPosition cursor{cursor_line_, cursor_column_};
  if (selection_anchor_->line == cursor.line && selection_anchor_->column == cursor.column) {
    return std::nullopt;
  }

  if (IsBefore(*selection_anchor_, cursor)) {
    return SelectionRange{*selection_anchor_, cursor};
  }
  return SelectionRange{cursor, *selection_anchor_};
}

std::string TextViewport::SelectedText() const {
  const auto range = selection_range();
  if (!range.has_value()) {
    return {};
  }

  const auto& start = range->start;
  const auto& end = range->end;
  if (start.line == end.line) {
    return document_->lines[start.line].substr(start.column, end.column - start.column);
  }

  std::size_t total_bytes = document_->lines[start.line].size() - start.column;
  for (std::size_t line = start.line + 1; line < end.line; ++line) {
    total_bytes += 1 + document_->lines[line].size();
  }
  total_bytes += 1 + end.column;

  std::string text;
  text.reserve(total_bytes);
  text += document_->lines[start.line].substr(start.column);
  text.push_back('\n');
  for (std::size_t line = start.line + 1; line < end.line; ++line) {
    text += document_->lines[line];
    text.push_back('\n');
  }
  text += document_->lines[end.line].substr(0, end.column);
  return text;
}

std::string TextViewport::CurrentLineTextForClipboard() const {
  if (document_->lines.empty()) {
    return {};
  }

  std::string text;
  text.reserve(document_->lines[cursor_line_].size() + 1);
  text += document_->lines[cursor_line_];
  text.push_back('\n');
  return text;
}

bool TextViewport::DeleteSelectedText() {
  return DeleteSelection();
}

bool TextViewport::DeleteCurrentLine() {
  if (document_->lines.empty()) {
    return false;
  }
  if (has_multiple_carets()) {
    std::vector<std::size_t> lines_to_delete;
    lines_to_delete.reserve(secondary_carets_.size() + 1);
    lines_to_delete.push_back(cursor_line_);
    for (const SecondaryCaret& caret : secondary_carets_) {
      lines_to_delete.push_back(std::min(caret.position.line, document_->lines.size() - 1));
    }
    std::sort(lines_to_delete.begin(), lines_to_delete.end());
    lines_to_delete.erase(std::unique(lines_to_delete.begin(), lines_to_delete.end()),
                          lines_to_delete.end());
    if (lines_to_delete.empty()) {
      return false;
    }

    const std::vector<std::string> before_lines = document_->lines;
    const ViewState before_state = CaptureViewState();
    for (auto it = lines_to_delete.rbegin(); it != lines_to_delete.rend(); ++it) {
      document_->lines.erase(document_->lines.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    if (document_->lines.empty()) {
      document_->lines.push_back("");
    }
    cursor_line_ = std::min(cursor_line_, document_->lines.size() - 1);
    cursor_column_ = 0;
    preferred_column_ = 0;
    selection_anchor_.reset();
    secondary_carets_.clear();
    document_->placeholder = false;
    document_->dirty = true;
    RefreshEncoding();
    InvalidateDerivedCaches(0);
    InvalidateVisualColumnCache();
    EnsureCursorVisible();
    PushHistoryEntry(BuildHistoryEntryForDocumentChange(before_lines, before_state,
                                                        document_->lines, CaptureViewState()));
    return true;
  }

  if (document_->lines.size() == 1) {
    return ApplyRangeEdit(SelectionRange{
                              .start = TextPosition{0, 0},
                              .end = TextPosition{0, document_->lines[0].size()},
                          },
                          "", true);
  }

  if (cursor_line_ + 1 < document_->lines.size()) {
    return ApplyRangeEdit(SelectionRange{
                              .start = TextPosition{cursor_line_, 0},
                              .end = TextPosition{cursor_line_ + 1, 0},
                          },
                          "", true);
  }

  const std::size_t previous_line = cursor_line_ - 1;
  return ApplyRangeEdit(SelectionRange{
                            .start = TextPosition{
                                previous_line,
                                document_->lines[previous_line].size(),
                            },
                            .end = TextPosition{cursor_line_, document_->lines[cursor_line_].size()},
                        },
                        "", true);
}

void TextViewport::ClearSelection() {
  selection_anchor_.reset();
}

void TextViewport::SelectAll() {
  if (document_->lines.empty()) {
    return;
  }

  selection_anchor_ = TextPosition{0, 0};
  cursor_line_ = document_->lines.size() - 1;
  cursor_column_ = document_->lines.back().size();
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  EnsureCursorVisible();
}

void TextViewport::SelectWordAtCursor() {
  if (document_->lines.empty()) {
    return;
  }
  const std::string& line = document_->lines[cursor_line_];
  const std::size_t col = std::min(cursor_column_, line.size());
  auto is_word_char = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };
  std::size_t start = col;
  std::size_t end = col;
  if (col < line.size() && is_word_char(line[col])) {
    while (start > 0 && is_word_char(line[start - 1])) {
      --start;
    }
    while (end < line.size() && is_word_char(line[end])) {
      ++end;
    }
  }
  if (start == end) {
    return;
  }
  selection_anchor_ = TextPosition{cursor_line_, start};
  cursor_column_ = end;
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  EnsureCursorVisible();
}

std::optional<SelectionRange> TextViewport::OccurrenceSeedSpanForHighlight() const {
  if (document_->lines.empty()) {
    return std::nullopt;
  }

  auto is_word_char = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };

  if (const auto selected = selection_range()) {
    if (selected->start.line != selected->end.line) {
      return std::nullopt;
    }
    const std::size_t line_index = selected->start.line;
    std::size_t start_col = selected->start.column;
    std::size_t end_col = selected->end.column;
    if (start_col > end_col) {
      std::swap(start_col, end_col);
    }
    if (start_col < end_col) {
      return SelectionRange{{line_index, start_col}, {line_index, end_col}};
    }
  }

  const std::size_t line_index = cursor_line_;
  const std::string& line = document_->lines[line_index];
  const std::size_t col = std::min(cursor_column_, line.size());
  std::size_t anchor_col = col;
  if (col < line.size() && is_word_char(line[col])) {
    // Primary caret indexes a word character.
  } else if (col > 0 && is_word_char(line[col - 1])) {
    anchor_col = col - 1;
  } else {
    return std::nullopt;
  }

  std::size_t start = anchor_col;
  std::size_t end = anchor_col;
  while (start > 0 && is_word_char(line[start - 1])) {
    --start;
  }
  while (end < line.size() && is_word_char(line[end])) {
    ++end;
  }
  if (start >= end) {
    return std::nullopt;
  }
  return SelectionRange{{line_index, start}, {line_index, end}};
}

void TextViewport::SelectLineAtCursor() {
  if (document_->lines.empty()) {
    return;
  }
  selection_anchor_ = TextPosition{cursor_line_, 0};
  cursor_column_ = document_->lines[cursor_line_].size();
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  EnsureCursorVisible();
}

void TextViewport::ResetState(std::vector<std::string> lines,
                              const std::filesystem::path& path,
                              LineEnding line_ending,
                              bool mixed_line_endings,
                              TextEncoding encoding,
                              bool placeholder,
                              bool dirty) {
  EnsureDocument();
  document_->path = path;
  document_->lines = lines.empty() ? std::vector<std::string>{""} : std::move(lines);
  document_->line_ending = line_ending;
  document_->mixed_line_endings = mixed_line_endings;
  document_->encoding = encoding;
  cursor_line_ = 0;
  cursor_column_ = 0;
  preferred_column_ = 0;
  scroll_line_ = 0;
  horizontal_scroll_ = 0;
  selection_anchor_.reset();
  secondary_carets_.clear();
  document_->undo_stack.clear();
  document_->redo_stack.clear();
  document_->placeholder = placeholder;
  document_->dirty = dirty;
  InvalidateVisualColumnCache();
  InvalidateDerivedCaches();
  // ResetState is a fresh load, not an in-place edit; clear the fold edit
  // anchor to the idle sentinel so the next user edit publishes its own
  // first-touched line instead of being masked by the wholesale reset.
  fold_edit_anchor_line_ = std::numeric_limits<std::size_t>::max();
  EnsureCursorVisible();
}

void TextViewport::EnsureInitialHighlightState() const {
  std::string perf_label = "TextViewport::EnsureInitialHighlightState";
  if (util::PerformanceTrace::Enabled() && !document_->path.empty()) {
    perf_label += "(path=" + document_->path.string() + ")";
  }
  util::PerformanceTrace::Scope perf_scope(perf_label);
  if (!syntax_highlighting_enabled()) {
    initial_highlight_state_.reset();
    return;
  }
  if (initial_highlight_state_.has_value()) {
    return;
  }
  initial_highlight_state_ = SyntaxHighlighter::InitialState(document_->path, document_->lines);
}

void TextViewport::EnsureHighlightCaches() const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::EnsureHighlightCaches");
  if (!syntax_highlighting_enabled() || document_->lines.empty()) {
    return;
  }

  EnsureInitialHighlightState();
  if (highlight_state_revision_ != document_->layout_revision) {
    line_highlight_states_.assign(document_->lines.size(), SyntaxState{});
    highlight_checkpoints_.clear();
    highlight_state_revision_ = document_->layout_revision;
  }
  if (line_highlight_states_.size() != document_->lines.size()) {
    line_highlight_states_.assign(document_->lines.size(), SyntaxState{});
  }
  const std::size_t checkpoint_count =
      ((document_->lines.size() - 1) / kHighlightCheckpointInterval) + 1;
  if (highlight_checkpoints_.size() != checkpoint_count) {
    highlight_checkpoints_.assign(checkpoint_count, SyntaxState{});
  }
  if (!highlight_checkpoints_.empty()) {
    highlight_checkpoints_.front() = *initial_highlight_state_;
  }
}

void TextViewport::EnsureHighlightCheckpoint(std::size_t checkpoint_index) const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::EnsureHighlightCheckpoint");
  EnsureHighlightCaches();
  if (!syntax_highlighting_enabled() || document_->lines.empty() ||
      checkpoint_index >= highlight_checkpoints_.size()) {
    return;
  }
  if (IsCachedHighlightState(highlight_checkpoints_[checkpoint_index])) {
    return;
  }

  std::size_t previous_checkpoint = checkpoint_index;
  while (previous_checkpoint > 0 &&
         !IsCachedHighlightState(highlight_checkpoints_[previous_checkpoint])) {
    --previous_checkpoint;
  }

  SyntaxState state = previous_checkpoint == 0
                          ? *initial_highlight_state_
                          : highlight_checkpoints_[previous_checkpoint];
  std::size_t line = previous_checkpoint * kHighlightCheckpointInterval;
  const std::size_t target_line =
      std::min(document_->lines.size(), checkpoint_index * kHighlightCheckpointInterval);
  util::PerformanceTrace::Scope replay_scope(
      "TextViewport::EnsureHighlightCheckpoint::ReplayToCheckpoint");
  for (; line < target_line; ++line) {
    if (IsCachedHighlightState(line_highlight_states_[line])) {
      state = line_highlight_states_[line];
    } else {
      {
        util::PerformanceTrace::Scope advance_scope(
            "TextViewport::EnsureHighlightCheckpoint::AdvanceState");
        state = SyntaxHighlighter::AdvanceState(document_->lines[line], document_->path, state);
      }
      line_highlight_states_[line] = state;
      ++highlight_checkpoint_advances_;
    }
    const std::size_t next_line = line + 1;
    if (next_line < document_->lines.size() &&
        next_line % kHighlightCheckpointInterval == 0) {
      highlight_checkpoints_[next_line / kHighlightCheckpointInterval] = state;
    }
  }
}

SyntaxState TextViewport::HighlightStateBeforeLine(std::size_t line_index) const {
  util::PerformanceTrace::Scope perf_scope("TextViewport::HighlightStateBeforeLine");
  EnsureHighlightCaches();
  if (line_index == 0) {
    return *initial_highlight_state_;
  }

  const std::size_t checkpoint_index = line_index / kHighlightCheckpointInterval;
  EnsureHighlightCheckpoint(checkpoint_index);
  const std::size_t checkpoint_line = checkpoint_index * kHighlightCheckpointInterval;
  SyntaxState state = checkpoint_index == 0
                          ? *initial_highlight_state_
                          : highlight_checkpoints_[checkpoint_index];

  util::PerformanceTrace::Scope replay_scope("TextViewport::HighlightStateBeforeLine::Replay");
  for (std::size_t line = checkpoint_line; line < line_index; ++line) {
    if (IsCachedHighlightState(line_highlight_states_[line])) {
      state = line_highlight_states_[line];
      continue;
    }
    {
      util::PerformanceTrace::Scope advance_scope(
          "TextViewport::HighlightStateBeforeLine::AdvanceState");
      state = SyntaxHighlighter::AdvanceState(document_->lines[line], document_->path, state);
    }
    line_highlight_states_[line] = state;
    ++highlight_state_advances_;
  }
  return state;
}

void TextViewport::InvalidateDerivedCaches() {
  InvalidateDerivedCaches(0);
}

void TextViewport::InvalidateDerivedCaches(std::size_t start_line) {
  EnsureDocument();
  ++document_->layout_revision;
  const std::size_t safe_start = std::min(start_line, document_->lines.size());
  util::AddPerformanceCounter(util::PerfCounterId::EditorInvalidateDerivedCachesCalls);
  util::AddPerformanceCounter(util::PerfCounterId::EditorInvalidateDerivedCachesLines,
                              document_->lines.size() - safe_start);

  // Folding incremental scan anchors at lines >= fold_edit_anchor_line_. Zero
  // forces a bracket rescan without prefix reuse; SIZE_MAX sentinel means idle.
  if (safe_start == 0) {
    fold_edit_anchor_line_ = 0;
  } else if (safe_start >= document_->lines.size()) {
    fold_edit_anchor_line_ = document_->lines.size();
  } else {
    fold_edit_anchor_line_ = std::min(fold_edit_anchor_line_, safe_start);
  }

  if (safe_start == 0) {
    visible_line_cache_.clear();
    visible_line_cache_order_.clear();
    highlight_cache_.clear();
    highlight_cache_order_.clear();
    initial_highlight_state_.reset();
    line_highlight_states_.clear();
    highlight_checkpoints_.clear();
    highlight_state_revision_ = document_->layout_revision;
    return;
  }

  for (auto it = visible_line_cache_.begin(); it != visible_line_cache_.end();) {
    if (it->first.line_index >= safe_start) {
      it = visible_line_cache_.erase(it);
    } else {
      ++it;
    }
  }
  visible_line_cache_order_.erase(
      std::remove_if(visible_line_cache_order_.begin(), visible_line_cache_order_.end(),
                     [&](const VisibleLineCacheKey& key) { return key.line_index >= safe_start; }),
      visible_line_cache_order_.end());

  for (auto it = highlight_cache_.begin(); it != highlight_cache_.end();) {
    if (it->first >= safe_start) {
      it = highlight_cache_.erase(it);
    } else {
      ++it;
    }
  }
  highlight_cache_order_.erase(
      std::remove_if(highlight_cache_order_.begin(), highlight_cache_order_.end(),
                     [&](std::size_t line_index) { return line_index >= safe_start; }),
      highlight_cache_order_.end());

  if (line_highlight_states_.size() != document_->lines.size()) {
    line_highlight_states_.resize(document_->lines.size(), SyntaxState{});
  }
  for (std::size_t line = safe_start; line < line_highlight_states_.size(); ++line) {
    line_highlight_states_[line] = SyntaxState{};
  }

  const std::size_t checkpoint_count =
      document_->lines.empty() ? 0
                               : ((document_->lines.size() - 1) / kHighlightCheckpointInterval) + 1;
  if (highlight_checkpoints_.size() != checkpoint_count) {
    highlight_checkpoints_.resize(checkpoint_count, SyntaxState{});
  }
  const std::size_t checkpoint_start = safe_start / kHighlightCheckpointInterval;
  for (std::size_t index = checkpoint_start; index < highlight_checkpoints_.size(); ++index) {
    highlight_checkpoints_[index] = SyntaxState{};
  }
  if (!highlight_checkpoints_.empty()) {
    EnsureInitialHighlightState();
    highlight_checkpoints_.front() = *initial_highlight_state_;
  }
  highlight_state_revision_ = document_->layout_revision;
}

std::size_t TextViewport::ConsumeFoldEditAnchorLine() {
  const std::size_t v = fold_edit_anchor_line_;
  fold_edit_anchor_line_ = std::numeric_limits<std::size_t>::max();
  return v;
}

void TextViewport::InvalidateVisualColumnCache() {
  cached_max_visual_columns_.reset();
  cached_max_visual_columns_line_index_.reset();
  cached_visual_line_columns_.clear();
  cached_max_visual_columns_tab_size_ = 0;
  cached_max_visual_columns_revision_ = 0;
  wrapped_row_layouts_.clear();
  wrapped_line_row_offsets_.clear();
  wrapped_row_layouts_tab_size_ = 0;
  wrapped_row_layouts_visible_columns_ = 0;
  wrapped_row_layouts_revision_ = 0;
  wrapped_row_layouts_soft_wrap_ = false;
  wrapped_row_layouts_folding_model_ = nullptr;
  wrapped_row_layouts_fold_revision_ = 0;
  wrapped_row_layouts_trivial_ = false;
}

void TextViewport::InvalidateLayoutCaches() {
  InvalidateVisualColumnCache();
  InvalidateDerivedCaches(0);
}

void TextViewport::InvalidateSyntaxHighlighting() {
  InvalidateLayoutCaches();
}

void TextViewport::RefreshEncoding() {
  util::AddPerformanceCounter(util::PerfCounterId::EditorRefreshEncodingCalls);
  document_->encoding = DetectEncoding(document_->lines);
}

void TextViewport::BeginSelectionIfNeeded(bool extend_selection) {
  if (extend_selection) {
    if (!selection_anchor_.has_value()) {
      selection_anchor_ = TextPosition{cursor_line_, cursor_column_};
    }
    return;
  }
  selection_anchor_.reset();
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

void TextViewport::PushHistoryEntry(HistoryEntry entry) {
  document_->redo_stack.clear();
  if (!undo_group_stack_.empty()) {
    for (UndoGroupFrame& frame : undo_group_stack_) {
      if (frame.using_fallback) {
        continue;
      }
      frame.child_entries.push_back(entry);
      if (!frame.aggregate_entry.has_value()) {
        frame.aggregate_entry = entry;
        continue;
      }
      std::optional<HistoryEntry> merged =
          TryMergeUndoGroupEntry(*frame.aggregate_entry, entry);
      if (merged.has_value()) {
        frame.aggregate_entry = std::move(merged);
        continue;
      }
      frame.fallback_lines =
          ReconstructUndoGroupFallbackLines(document_->lines, frame.child_entries);
      frame.aggregate_entry.reset();
      frame.child_entries.clear();
      frame.using_fallback = true;
    }
    return;
  }
  document_->undo_stack.push_back(std::move(entry));
  if (document_->undo_stack.size() > kMaxHistoryEntries) {
    document_->undo_stack.pop_front();
  }
}

void TextViewport::PushHistoryEntryDirect(HistoryEntry entry) {
  document_->redo_stack.clear();
  document_->undo_stack.push_back(std::move(entry));
  if (document_->undo_stack.size() > kMaxHistoryEntries) {
    document_->undo_stack.pop_front();
  }
}

void TextViewport::BeginUndoGroup() {
  UndoGroupFrame frame;
  frame.state = CaptureViewState();
  undo_group_stack_.push_back(std::move(frame));
}

void TextViewport::EndUndoGroup() {
  FlushActiveUndoGroup();
}

void TextViewport::FlushActiveUndoGroup() {
  if (undo_group_stack_.empty()) {
    return;
  }
  UndoGroupFrame frame = std::move(undo_group_stack_.back());
  undo_group_stack_.pop_back();

  HistoryEntry agg;
  if (frame.using_fallback) {
    // Conservative fallback for grouped edits whose child deltas cannot be
    // merged into one contiguous history entry.
    agg = BuildHistoryEntryForDocumentChange(frame.fallback_lines, frame.state,
                                             document_->lines, CaptureViewState());
  } else if (frame.aggregate_entry.has_value()) {
    agg = *frame.aggregate_entry;
    agg.before_state = frame.state;
    agg.after_state = CaptureViewState();
  } else {
    // No edits happened. Build an empty aggregate so the no-op check below
    // can discard it uniformly.
    agg = BuildHistoryEntryForDocumentChange({}, frame.state, {}, CaptureViewState());
  }

  if (agg.before_lines.empty() && agg.after_lines.empty()) {
    return;
  }
  PushHistoryEntry(std::move(agg));
}

void TextViewport::ApplyHistoryEntry(const HistoryEntry& entry, bool forward) {
  const std::size_t start_line = std::min(entry.start_line, document_->lines.size());
  const std::size_t removed_count = forward ? entry.before_lines.size() : entry.after_lines.size();
  const auto& inserted_lines = forward ? entry.after_lines : entry.before_lines;
  ApplyHistoryEntryToLines(document_->lines, entry, forward);

  RestoreViewState(forward ? entry.after_state : entry.before_state);
  RefreshEncoding();
  InvalidateDerivedCaches(start_line);
  UpdateVisualColumnCacheAfterEdit(start_line, removed_count, inserted_lines);
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

  const std::vector<std::string> before_lines =
      SliceLines(document_->lines, start.line, end.line + 1);
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
    after_lines.insert(after_lines.end(), replacement_lines.begin() + 1, replacement_lines.end() - 1);
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
      .before_lines = before_lines,
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
      .before_lines = SliceLines(document_->lines, clamped_start, clamped_end),
      .after_lines = std::move(after_lines),
      .before_state = CaptureViewState(),
      .after_state = after_state,
  };
}

TextViewport::HistoryEntry TextViewport::BuildHistoryEntryForDocumentChange(
    const std::vector<std::string>& before_lines,
    const ViewState& before_state,
    const std::vector<std::string>& after_lines,
    const ViewState& after_state) {
  std::size_t prefix = 0;
  while (prefix < before_lines.size() && prefix < after_lines.size() &&
         before_lines[prefix] == after_lines[prefix]) {
    ++prefix;
  }

  std::size_t before_end = before_lines.size();
  std::size_t after_end = after_lines.size();
  while (before_end > prefix && after_end > prefix &&
         before_lines[before_end - 1] == after_lines[after_end - 1]) {
    --before_end;
    --after_end;
  }

  return HistoryEntry{
      .start_line = prefix,
      .before_lines = SliceLines(before_lines, prefix, before_end),
      .after_lines = SliceLines(after_lines, prefix, after_end),
      .before_state = before_state,
      .after_state = after_state,
  };
}

std::optional<AppliedEdit> TextViewport::BuildAppliedEditForHistoryEntry(
    const TextViewport::HistoryEntry& entry,
    bool forward) {
  const std::vector<std::string>& before_lines = forward ? entry.before_lines : entry.after_lines;
  const std::vector<std::string>& after_lines = forward ? entry.after_lines : entry.before_lines;
  if (before_lines.empty() || after_lines.empty()) {
    return std::nullopt;
  }

  const std::string& before_first = before_lines.front();
  const std::string& after_first = after_lines.front();
  std::size_t common_prefix = 0;
  const std::size_t max_prefix = std::min(before_first.size(), after_first.size());
  while (common_prefix < max_prefix && before_first[common_prefix] == after_first[common_prefix]) {
    ++common_prefix;
  }

  const std::string& before_last = before_lines.back();
  const std::string& after_last = after_lines.back();
  std::size_t common_suffix = 0;
  const std::size_t max_suffix = std::min(before_last.size(), after_last.size());
  while (common_suffix < max_suffix &&
         before_last[before_last.size() - 1 - common_suffix] ==
             after_last[after_last.size() - 1 - common_suffix]) {
    if ((before_lines.size() == 1 && common_prefix + common_suffix >= before_first.size()) ||
        (after_lines.size() == 1 && common_prefix + common_suffix >= after_first.size())) {
      break;
    }
    ++common_suffix;
  }

  std::vector<std::string> replacement_lines = after_lines;
  replacement_lines.front().erase(0, common_prefix);
  if (common_suffix > 0) {
    replacement_lines.back().erase(replacement_lines.back().size() - common_suffix);
  }

  return AppliedEdit{
      .range_before =
          SelectionRange{
              .start =
                  TextPosition{
                      .line = entry.start_line,
                      .column = common_prefix,
                  },
              .end =
                  TextPosition{
                      .line = entry.start_line + before_lines.size() - 1,
                      .column = before_last.size() - common_suffix,
                  },
          },
      .replacement_text = util::SerializeLines(replacement_lines, util::LineEnding::LF),
  };
}

void TextViewport::ApplyHistoryEntryToLines(std::vector<std::string>& lines,
                                            const HistoryEntry& entry,
                                            bool forward) {
  const std::size_t start_line = std::min(entry.start_line, lines.size());
  const std::size_t removed_count = forward ? entry.before_lines.size() : entry.after_lines.size();
  const auto& inserted_lines = forward ? entry.after_lines : entry.before_lines;

  const bool same_count_replacement =
      removed_count > 0 && removed_count == inserted_lines.size() &&
      start_line + removed_count <= lines.size();
  if (same_count_replacement) {
    for (std::size_t i = 0; i < removed_count; ++i) {
      lines[start_line + i] = inserted_lines[i];
    }
  } else {
    const auto erase_begin = lines.begin() + static_cast<std::ptrdiff_t>(start_line);
    const auto erase_end = erase_begin +
                           static_cast<std::ptrdiff_t>(std::min(removed_count, lines.size() - start_line));
    lines.erase(erase_begin, erase_end);
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(start_line),
                 inserted_lines.begin(), inserted_lines.end());
  }
  if (lines.empty()) {
    lines.push_back("");
  }
}

std::optional<TextViewport::HistoryEntry> TextViewport::TryMergeUndoGroupEntry(
    const HistoryEntry& aggregate,
    const HistoryEntry& next) {
  const std::size_t aggregate_after_start = aggregate.start_line;
  const std::size_t aggregate_after_end = aggregate.start_line + aggregate.after_lines.size();
  const std::size_t next_start = next.start_line;
  const std::size_t next_end = next.start_line + next.before_lines.size();

  HistoryEntry merged = aggregate;
  merged.after_state = next.after_state;

  if (next_end == aggregate_after_start) {
    merged.start_line = next.start_line;
    merged.before_lines = next.before_lines;
    merged.before_lines.insert(merged.before_lines.end(), aggregate.before_lines.begin(),
                               aggregate.before_lines.end());
    merged.after_lines = next.after_lines;
    merged.after_lines.insert(merged.after_lines.end(), aggregate.after_lines.begin(),
                              aggregate.after_lines.end());
    return merged;
  }

  if (next_start == aggregate_after_end) {
    merged.before_lines.insert(merged.before_lines.end(), next.before_lines.begin(),
                               next.before_lines.end());
    merged.after_lines.insert(merged.after_lines.end(), next.after_lines.begin(),
                              next.after_lines.end());
    return merged;
  }

  if (next_start < aggregate_after_start || next_end > aggregate_after_end) {
    return std::nullopt;
  }

  const std::size_t relative_start = next_start - aggregate_after_start;
  const std::size_t relative_end = relative_start + next.before_lines.size();
  if (!std::equal(next.before_lines.begin(), next.before_lines.end(),
                  aggregate.after_lines.begin() + static_cast<std::ptrdiff_t>(relative_start),
                  aggregate.after_lines.begin() + static_cast<std::ptrdiff_t>(relative_end))) {
    return std::nullopt;
  }

  merged.after_lines.erase(
      merged.after_lines.begin() + static_cast<std::ptrdiff_t>(relative_start),
      merged.after_lines.begin() + static_cast<std::ptrdiff_t>(relative_end));
  merged.after_lines.insert(
      merged.after_lines.begin() + static_cast<std::ptrdiff_t>(relative_start),
      next.after_lines.begin(), next.after_lines.end());
  return merged;
}

std::vector<std::string> TextViewport::ReconstructUndoGroupFallbackLines(
    const std::vector<std::string>& current_lines,
    const std::vector<HistoryEntry>& child_entries) {
  std::vector<std::string> reconstructed = current_lines;
  for (auto it = child_entries.rbegin(); it != child_entries.rend(); ++it) {
    ApplyHistoryEntryToLines(reconstructed, *it, false);
  }
  return reconstructed;
}

bool TextViewport::ApplyMultiCaretInsert(std::string_view text, bool record_undo) {
  last_applied_edit_.reset();
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  std::vector<TextPosition> carets = secondary_carets();
  carets.push_back(TextPosition{cursor_line_, cursor_column_});
  std::sort(carets.begin(), carets.end(), TextPositionLess);
  carets.erase(std::unique(carets.begin(), carets.end()), carets.end());
  if (carets.empty()) {
    return false;
  }

  const std::vector<std::string> before_lines = document_->lines;
  const ViewState before_state = CaptureViewState();
  const TextPosition primary_before{cursor_line_, cursor_column_};
  TextPosition primary_after = primary_before;
  std::vector<TextPosition> updated_secondary_carets;
  updated_secondary_carets.reserve(carets.size());

  for (auto it = carets.rbegin(); it != carets.rend(); ++it) {
    const std::size_t line = std::min(it->line, document_->lines.size() - 1);
    const std::size_t column = TextLayout::ClampTextColumn(document_->lines[line], it->column);
    const std::string replacement =
        text == "\n" ? "\n" + AutoIndentForNewline(line, column) : std::string(text);
    const std::optional<HistoryEntry> entry = BuildRangeHistoryEntry(
        SelectionRange{TextPosition{line, column}, TextPosition{line, column}}, replacement);
    if (!entry.has_value()) {
      if (!(line == primary_before.line && column == primary_before.column)) {
        updated_secondary_carets.push_back(TextPosition{line, column});
      }
      continue;
    }
    ApplyHistoryEntry(*entry, true);
    const TextPosition updated_position{
        entry->after_state.cursor_line,
        entry->after_state.cursor_column,
    };
    if (line == primary_before.line && column == primary_before.column) {
      primary_after = updated_position;
    } else {
      updated_secondary_carets.push_back(updated_position);
    }
  }

  cursor_line_ = primary_after.line;
  cursor_column_ = primary_after.column;
  std::sort(updated_secondary_carets.begin(), updated_secondary_carets.end(), TextPositionLess);
  updated_secondary_carets.erase(
      std::unique(updated_secondary_carets.begin(), updated_secondary_carets.end()),
      updated_secondary_carets.end());
  updated_secondary_carets.erase(
      std::remove(updated_secondary_carets.begin(), updated_secondary_carets.end(), primary_after),
      updated_secondary_carets.end());
  secondary_carets_.clear();
  for (const TextPosition& caret : updated_secondary_carets) {
    AddSecondaryCaret(caret.line, caret.column);
  }
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  document_->placeholder = false;
  document_->dirty = true;
  EnsureCursorVisible();

  const HistoryEntry aggregate_entry =
      BuildHistoryEntryForDocumentChange(before_lines, before_state, document_->lines, CaptureViewState());
  last_applied_edit_ = BuildAppliedEditForHistoryEntry(aggregate_entry, true);
  if (record_undo) {
    PushHistoryEntry(aggregate_entry);
  } else {
    document_->redo_stack.clear();
  }
  return true;
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
  if (!ValidateRangeColumns(document_->lines, norm)) {
    return false;
  }
  if (InInsertionSuppressedScope(norm.start.line, norm.start.column)) {
    return false;
  }

  const std::string inner = TextBetweenLines(document_->lines, norm);
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
      return RangeEndExclusive(*s.selection);
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
    slots.push_back(Slot{.is_primary = false,
                         .secondary_index = i,
                         .reference = sc.position,
                         .selection =
                             SelectionRangeForSecondaryCaret(sc.position, sc.selection_anchor)});
  }
  std::sort(slots.begin(), slots.end(),
            [&](const Slot& a, const Slot& b) { return TextPositionLess(slot_sort_end(a), slot_sort_end(b)); });

  // Common multi-caret pair-insert paths (surround, auto-close, plain char
  // insert) preserve line count when no selection spans multiple lines and the
  // inserted glyph is not a newline. Capture only the touched line range
  // instead of snapshotting the entire document; the aggregate undo entry is
  // built from this slice and offset back into document coordinates.
  bool slice_safe = (ch != '\n');
  std::size_t slice_min_line = std::numeric_limits<std::size_t>::max();
  std::size_t slice_max_line = 0;
  if (slice_safe) {
    for (const Slot& slot : slots) {
      std::size_t lo = 0;
      std::size_t hi = 0;
      if (slot.selection.has_value()) {
        const SelectionRange norm = NormalizeRange(*slot.selection);
        if (norm.start.line != norm.end.line) {
          slice_safe = false;
          break;
        }
        lo = std::min(norm.start.line, norm.end.line);
        hi = std::max(norm.start.line, norm.end.line);
      } else {
        if (document_->lines.empty()) {
          slice_safe = false;
          break;
        }
        lo = std::min(slot.reference.line, document_->lines.size() - 1);
        hi = lo;
      }
      slice_min_line = std::min(slice_min_line, lo);
      slice_max_line = std::max(slice_max_line, hi);
    }
    if (slice_min_line == std::numeric_limits<std::size_t>::max()) {
      slice_safe = false;
    }
  }

  std::vector<std::string> before_lines;
  std::size_t before_lines_start = 0;
  if (slice_safe) {
    slice_max_line = std::min(slice_max_line, document_->lines.size() - 1);
    before_lines_start = slice_min_line;
    before_lines.reserve(slice_max_line - slice_min_line + 1);
    for (std::size_t i = slice_min_line; i <= slice_max_line; ++i) {
      before_lines.push_back(document_->lines[i]);
    }
  } else {
    before_lines = document_->lines;
  }
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
      if (!ValidateRangeColumns(document_->lines, norm)) {
        if (!slot.is_primary) {
          // leave new_secondaries unchanged for this index
        }
        continue;
      }

      const bool sel_multi = (norm.start.line != norm.end.line);
      const auto* sur_pair =
          lc_view_.surround_enabled ? FindSurroundOpener(lc_view_, ch) : nullptr;
      if (sur_pair != nullptr && !sur_pair->open.empty() && !sur_pair->close.empty() && !sel_multi &&
          !InInsertionSuppressedScope(norm.start.line, norm.start.column)) {
        const std::string inner = TextBetweenLines(document_->lines, norm);
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
              return TextPositionLess(lhs.position, rhs.position);
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
  HistoryEntry aggregate_entry;
  if (slice_safe) {
    std::vector<std::string> after_lines_slice;
    after_lines_slice.reserve(before_lines.size());
    const std::size_t slice_end =
        std::min(before_lines_start + before_lines.size(), document_->lines.size());
    for (std::size_t i = before_lines_start; i < slice_end; ++i) {
      after_lines_slice.push_back(document_->lines[i]);
    }
    aggregate_entry =
        BuildHistoryEntryForDocumentChange(before_lines, before_state,
                                           after_lines_slice, CaptureViewState());
    aggregate_entry.start_line += before_lines_start;
  } else {
    aggregate_entry = BuildHistoryEntryForDocumentChange(before_lines, before_state,
                                                         document_->lines, CaptureViewState());
  }
  last_applied_edit_ = BuildAppliedEditForHistoryEntry(aggregate_entry, true);
  PushHistoryEntry(aggregate_entry);
  return true;
}

bool TextViewport::ApplyMultiCaretBackspace(bool record_undo) {
  last_applied_edit_.reset();
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  std::vector<TextPosition> carets = secondary_carets();
  carets.push_back(TextPosition{cursor_line_, cursor_column_});
  std::sort(carets.begin(), carets.end(), TextPositionLess);
  carets.erase(std::unique(carets.begin(), carets.end()), carets.end());
  if (carets.empty()) {
    return false;
  }

  const std::vector<std::string> before_lines = document_->lines;
  const ViewState before_state = CaptureViewState();
  const TextPosition primary_before{cursor_line_, cursor_column_};
  TextPosition primary_after = primary_before;
  std::vector<TextPosition> updated_secondary_carets;
  updated_secondary_carets.reserve(carets.size());
  bool changed = false;

  for (auto it = carets.rbegin(); it != carets.rend(); ++it) {
    const std::size_t line = std::min(it->line, document_->lines.size() - 1);
    const std::size_t column = TextLayout::ClampTextColumn(document_->lines[line], it->column);
    std::optional<HistoryEntry> entry;
    if (column > 0) {
      const std::size_t erase_start =
          TextLayout::PreviousTextColumn(document_->lines[line], column);
      entry = BuildRangeHistoryEntry(
          SelectionRange{TextPosition{line, erase_start}, TextPosition{line, column}}, "");
    } else if (line > 0) {
      entry = BuildRangeHistoryEntry(SelectionRange{
                                         TextPosition{line - 1, document_->lines[line - 1].size()},
                                         TextPosition{line, 0},
                                     },
                                     "");
    }
    if (!entry.has_value()) {
      if (!(line == primary_before.line && column == primary_before.column)) {
        updated_secondary_carets.push_back(TextPosition{line, column});
      }
      continue;
    }
    changed = true;
    ApplyHistoryEntry(*entry, true);
    const TextPosition updated_position{
        entry->after_state.cursor_line,
        entry->after_state.cursor_column,
    };
    if (line == primary_before.line && column == primary_before.column) {
      primary_after = updated_position;
    } else {
      updated_secondary_carets.push_back(updated_position);
    }
  }

  if (!changed) {
    return false;
  }

  cursor_line_ = primary_after.line;
  cursor_column_ = primary_after.column;
  std::sort(updated_secondary_carets.begin(), updated_secondary_carets.end(), TextPositionLess);
  updated_secondary_carets.erase(
      std::unique(updated_secondary_carets.begin(), updated_secondary_carets.end()),
      updated_secondary_carets.end());
  updated_secondary_carets.erase(
      std::remove(updated_secondary_carets.begin(), updated_secondary_carets.end(), primary_after),
      updated_secondary_carets.end());
  secondary_carets_.clear();
  for (const TextPosition& caret : updated_secondary_carets) {
    AddSecondaryCaret(caret.line, caret.column);
  }
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  document_->placeholder = false;
  document_->dirty = true;
  EnsureCursorVisible();

  const HistoryEntry aggregate_entry =
      BuildHistoryEntryForDocumentChange(before_lines, before_state, document_->lines, CaptureViewState());
  last_applied_edit_ = BuildAppliedEditForHistoryEntry(aggregate_entry, true);
  if (record_undo) {
    PushHistoryEntry(aggregate_entry);
  } else {
    document_->redo_stack.clear();
  }
  return true;
}

bool TextViewport::ApplyMultiCaretDeleteForward(bool record_undo) {
  last_applied_edit_.reset();
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  std::vector<TextPosition> carets = secondary_carets();
  carets.push_back(TextPosition{cursor_line_, cursor_column_});
  std::sort(carets.begin(), carets.end(), TextPositionLess);
  carets.erase(std::unique(carets.begin(), carets.end()), carets.end());
  if (carets.empty()) {
    return false;
  }

  const std::vector<std::string> before_lines = document_->lines;
  const ViewState before_state = CaptureViewState();
  const TextPosition primary_before{cursor_line_, cursor_column_};
  TextPosition primary_after = primary_before;
  std::vector<TextPosition> updated_secondary_carets;
  updated_secondary_carets.reserve(carets.size());
  bool changed = false;

  for (auto it = carets.rbegin(); it != carets.rend(); ++it) {
    const std::size_t line = std::min(it->line, document_->lines.size() - 1);
    const std::size_t column = TextLayout::ClampTextColumn(document_->lines[line], it->column);
    std::optional<HistoryEntry> entry;
    if (column < document_->lines[line].size()) {
      const std::size_t erase_end =
          TextLayout::NextTextColumn(document_->lines[line], column);
      entry = BuildRangeHistoryEntry(
          SelectionRange{TextPosition{line, column}, TextPosition{line, erase_end}}, "");
    } else if (line + 1 < document_->lines.size()) {
      entry = BuildRangeHistoryEntry(
          SelectionRange{TextPosition{line, column}, TextPosition{line + 1, 0}}, "");
    }
    if (!entry.has_value()) {
      if (!(line == primary_before.line && column == primary_before.column)) {
        updated_secondary_carets.push_back(TextPosition{line, column});
      }
      continue;
    }
    changed = true;
    ApplyHistoryEntry(*entry, true);
    const TextPosition updated_position{
        entry->after_state.cursor_line,
        entry->after_state.cursor_column,
    };
    if (line == primary_before.line && column == primary_before.column) {
      primary_after = updated_position;
    } else {
      updated_secondary_carets.push_back(updated_position);
    }
  }

  if (!changed) {
    return false;
  }

  cursor_line_ = primary_after.line;
  cursor_column_ = primary_after.column;
  std::sort(updated_secondary_carets.begin(), updated_secondary_carets.end(), TextPositionLess);
  updated_secondary_carets.erase(
      std::unique(updated_secondary_carets.begin(), updated_secondary_carets.end()),
      updated_secondary_carets.end());
  updated_secondary_carets.erase(
      std::remove(updated_secondary_carets.begin(), updated_secondary_carets.end(), primary_after),
      updated_secondary_carets.end());
  secondary_carets_.clear();
  for (const TextPosition& caret : updated_secondary_carets) {
    AddSecondaryCaret(caret.line, caret.column);
  }
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  selection_anchor_.reset();
  document_->placeholder = false;
  document_->dirty = true;
  EnsureCursorVisible();

  const HistoryEntry aggregate_entry =
      BuildHistoryEntryForDocumentChange(before_lines, before_state, document_->lines, CaptureViewState());
  last_applied_edit_ = BuildAppliedEditForHistoryEntry(aggregate_entry, true);
  if (record_undo) {
    PushHistoryEntry(aggregate_entry);
  } else {
    document_->redo_stack.clear();
  }
  return true;
}

bool TextViewport::ApplyRangeEdit(const SelectionRange& range,
                                  std::string_view replacement,
                                  bool record_undo) {
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  const std::optional<HistoryEntry> entry = BuildRangeHistoryEntry(range, replacement);
  if (!entry.has_value()) {
    last_applied_edit_.reset();
    return false;
  }

  ApplyHistoryEntry(*entry, true);
  last_applied_edit_ = BuildAppliedEditForHistoryEntry(*entry, true);
  if (record_undo) {
    HistoryEntry saved_entry = *entry;
    saved_entry.after_state = CaptureViewState();
    PushHistoryEntry(std::move(saved_entry));
  } else {
    document_->redo_stack.clear();
  }
  return true;
}

bool TextViewport::ApplyLineEdit(std::size_t start_line,
                                 std::size_t end_line,
                                 const std::vector<std::string>& replacement,
                                 bool record_undo) {
  EnsureDocument();
  if (document_->lines.empty()) {
    document_->lines.push_back("");
  }

  const HistoryEntry entry = BuildLineHistoryEntry(start_line, end_line, replacement);
  ApplyHistoryEntry(entry, true);
  last_applied_edit_ = BuildAppliedEditForHistoryEntry(entry, true);
  if (record_undo) {
    HistoryEntry saved_entry = entry;
    saved_entry.after_state = CaptureViewState();
    PushHistoryEntry(std::move(saved_entry));
  } else {
    document_->redo_stack.clear();
  }
  return true;
}

std::size_t TextViewport::CurrentLineLength() const {
  if (document_->lines.empty() || cursor_line_ >= document_->lines.size()) {
    return 0;
  }
  return document_->lines[cursor_line_].size();
}

void TextViewport::UpdateVisualColumnCacheAfterEdit(std::size_t start_line,
                                                    std::size_t removed_count,
                                                    const std::vector<std::string>& inserted_lines) {
  if (cached_max_visual_columns_tab_size_ != tab_size_ ||
      cached_visual_line_columns_.size() != document_->lines.size() - inserted_lines.size() +
                                               removed_count) {
    InvalidateVisualColumnCache();
    return;
  }

  std::vector<std::size_t> inserted_columns;
  inserted_columns.reserve(inserted_lines.size());
  for (const std::string& line : inserted_lines) {
    inserted_columns.push_back(TextLayout::VisualColumnForTextColumn(line, line.size(), tab_size_));
  }

  const std::size_t clamped_start = std::min(start_line, cached_visual_line_columns_.size());
  const std::size_t erase_end = std::min(clamped_start + removed_count, cached_visual_line_columns_.size());
  cached_visual_line_columns_.erase(
      cached_visual_line_columns_.begin() + static_cast<std::ptrdiff_t>(clamped_start),
      cached_visual_line_columns_.begin() + static_cast<std::ptrdiff_t>(erase_end));
  cached_visual_line_columns_.insert(
      cached_visual_line_columns_.begin() + static_cast<std::ptrdiff_t>(clamped_start),
      inserted_columns.begin(), inserted_columns.end());

  const bool max_line_erased =
      cached_max_visual_columns_line_index_.has_value() &&
      *cached_max_visual_columns_line_index_ >= clamped_start &&
      *cached_max_visual_columns_line_index_ < clamped_start + removed_count;
  const bool candidate_expands_max =
      std::any_of(inserted_columns.begin(), inserted_columns.end(), [&](std::size_t width) {
        return !cached_max_visual_columns_.has_value() || width >= *cached_max_visual_columns_;
      });
  if (max_line_erased || candidate_expands_max || !cached_max_visual_columns_.has_value()) {
    cached_max_visual_columns_.reset();
    cached_max_visual_columns_line_index_.reset();
  } else if (cached_max_visual_columns_line_index_.has_value() &&
             *cached_max_visual_columns_line_index_ >= clamped_start) {
    const std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(inserted_columns.size()) -
                                 static_cast<std::ptrdiff_t>(removed_count);
    *cached_max_visual_columns_line_index_ = static_cast<std::size_t>(
        static_cast<std::ptrdiff_t>(*cached_max_visual_columns_line_index_) + delta);
  }
  cached_max_visual_columns_revision_ = document_->layout_revision;
}

void TextViewport::ClampCursorColumn() {
  if (document_->lines.empty() || cursor_line_ >= document_->lines.size()) {
    cursor_column_ = 0;
    return;
  }

  cursor_column_ = TextLayout::TextColumnForVisualColumn(document_->lines[cursor_line_],
                                                         preferred_column_, tab_size_);
}

void TextViewport::ClampScrollState() {
  const std::size_t total_visual_lines = visual_line_count();
  const std::size_t max_vertical_scroll =
      total_visual_lines > visible_lines_ ? total_visual_lines - visible_lines_ : 0;
  scroll_line_ = std::min(scroll_line_, max_vertical_scroll);

  if (soft_wrap_) {
    horizontal_scroll_ = 0;
    return;
  }
  const std::size_t max_visual_columns = MaxVisualColumns();
  const std::size_t max_horizontal_scroll =
      max_visual_columns > visible_columns_ ? max_visual_columns - visible_columns_ : 0;
  horizontal_scroll_ = std::min(horizontal_scroll_, max_horizontal_scroll);
}

void TextViewport::EnsureCursorVisible() {
  const std::size_t cursor_visual_row = CursorVisualRow();
  if (cursor_visual_row < scroll_line_) {
    scroll_line_ = cursor_visual_row;
  }

  const std::size_t vertical_margin = std::min(kScrollMargin, visible_lines_ > 0 ? visible_lines_ - 1 : 0);
  if (cursor_visual_row < scroll_line_ + vertical_margin) {
    scroll_line_ = cursor_visual_row > vertical_margin ? cursor_visual_row - vertical_margin : 0;
  } else {
    const std::size_t visible_span = visible_lines_ > vertical_margin ? visible_lines_ - vertical_margin - 1 : 0;
    if (cursor_visual_row > scroll_line_ + visible_span) {
      scroll_line_ = cursor_visual_row > visible_span ? cursor_visual_row - visible_span : 0;
    }
  }

  if (soft_wrap_) {
    ClampScrollState();
    return;
  }

  const std::size_t visual_cursor_column = cursor_visual_column();
  if (visual_cursor_column < horizontal_scroll_ + kHorizontalScrollMargin) {
    horizontal_scroll_ =
        visual_cursor_column > kHorizontalScrollMargin ? visual_cursor_column - kHorizontalScrollMargin : 0;
    return;
  }

  const std::size_t visible_width = visible_columns_ > 0 ? visible_columns_ : 1;
  const std::size_t horizontal_span =
      visible_width > kHorizontalScrollMargin ? visible_width - kHorizontalScrollMargin - 1 : 0;
  if (visual_cursor_column > horizontal_scroll_ + horizontal_span) {
    horizontal_scroll_ =
        visual_cursor_column > horizontal_span ? visual_cursor_column - horizontal_span : 0;
  }

  ClampScrollState();
}

std::size_t TextViewport::MaxVisualColumns() const {
  if (cached_max_visual_columns_.has_value() &&
      cached_max_visual_columns_tab_size_ == tab_size_ &&
      cached_max_visual_columns_revision_ == document_->layout_revision) {
    return *cached_max_visual_columns_;
  }

  if (cached_max_visual_columns_tab_size_ != tab_size_ ||
      cached_visual_line_columns_.size() != document_->lines.size()) {
    cached_visual_line_columns_.assign(document_->lines.size(), 0);
    for (std::size_t index = 0; index < document_->lines.size(); ++index) {
      cached_visual_line_columns_[index] =
          TextLayout::VisualColumnForTextColumn(document_->lines[index],
                                                document_->lines[index].size(), tab_size_);
    }
  }

  std::size_t max_columns = 0;
  std::size_t max_line = 0;
  for (std::size_t index = 0; index < cached_visual_line_columns_.size(); ++index) {
    if (cached_visual_line_columns_[index] >= max_columns) {
      max_columns = cached_visual_line_columns_[index];
      max_line = index;
    }
  }
  cached_max_visual_columns_ = max_columns;
  cached_max_visual_columns_line_index_ = max_line;
  cached_max_visual_columns_tab_size_ = tab_size_;
  cached_max_visual_columns_revision_ = document_->layout_revision;
  return *cached_max_visual_columns_;
}

void TextViewport::EnsureWrappedRowLayouts() const {
  if (!document_) {
    return;
  }
  if (wrapped_row_layouts_revision_ == document_->layout_revision &&
      wrapped_row_layouts_tab_size_ == tab_size_ &&
      wrapped_row_layouts_visible_columns_ == visible_columns_ &&
      wrapped_row_layouts_soft_wrap_ == soft_wrap_ &&
      wrapped_row_layouts_folding_model_ == folding_model_ &&
      wrapped_row_layouts_fold_revision_ ==
          (folding_model_ != nullptr ? folding_model_->revision() : 0)) {
    return;
  }

  wrapped_row_layouts_.clear();
  wrapped_line_row_offsets_.clear();
  util::AddPerformanceCounter(util::PerfCounterId::EditorEnsureWrappedRowLayoutsRebuilds);
  // Probe whether the folding model has any collapsed range. When none exist
  // (the common no-folds path on a freshly-opened large file), skip the
  // per-line `IsLineHidden` query entirely. O(1) via the maintained counter
  // — the previous std::vector<bool> linear scan was paid on every edit.
  const bool has_any_collapsed_fold =
      folding_model_ != nullptr && folding_model_->has_any_collapsed_fold();

  // Trivial-layout fast path: visual row index equals document line index, the
  // visual window is the same for every row, and no line is hidden. Skip the
  // O(line_count) vector population entirely and have the readers synthesize
  // the row data inline. This is the steady state for a freshly-opened large
  // file with soft-wrap off; the previous implementation paid an O(line_count)
  // rebuild on every keystroke under exactly those conditions.
  if (!soft_wrap_ && !has_any_collapsed_fold) {
    wrapped_row_layouts_trivial_ = true;
    util::AddPerformanceCounter(util::PerfCounterId::EditorEnsureWrappedRowLayoutsLineVisits, 0);
    wrapped_row_layouts_tab_size_ = tab_size_;
    wrapped_row_layouts_visible_columns_ = visible_columns_;
    wrapped_row_layouts_revision_ = document_->layout_revision;
    wrapped_row_layouts_soft_wrap_ = soft_wrap_;
    wrapped_row_layouts_folding_model_ = folding_model_;
    wrapped_row_layouts_fold_revision_ =
        folding_model_ != nullptr ? folding_model_->revision() : 0;
#ifndef NDEBUG
    ++wrapped_row_layout_build_count_;
#endif
    return;
  }

  wrapped_row_layouts_trivial_ = false;
  wrapped_row_layouts_.reserve(document_->lines.size());
  wrapped_line_row_offsets_.reserve(document_->lines.size());
  util::AddPerformanceCounter(util::PerfCounterId::EditorEnsureWrappedRowLayoutsLineVisits,
                              document_->lines.size());
  const std::size_t wrap_columns = std::max<std::size_t>(1, visible_columns_);
  std::size_t last_visible_row = 0;
  if (!soft_wrap_) {
    // Non-soft-wrap with collapsed folds: still need wrapped_line_row_offsets_
    // so we can skip hidden lines, but the row payload is uniform.
    const WrappedRowLayout row_template{0, horizontal_scroll_,
                                         horizontal_scroll_ + visible_columns_};
    for (std::size_t line_index = 0; line_index < document_->lines.size(); ++line_index) {
      if (has_any_collapsed_fold && folding_model_->IsLineHidden(line_index)) {
        wrapped_line_row_offsets_.push_back(last_visible_row);
        continue;
      }
      wrapped_line_row_offsets_.push_back(wrapped_row_layouts_.size());
      last_visible_row = wrapped_row_layouts_.size();
      WrappedRowLayout row = row_template;
      row.line_index = line_index;
      wrapped_row_layouts_.push_back(row);
    }
  } else {
    for (std::size_t line_index = 0; line_index < document_->lines.size(); ++line_index) {
      if (has_any_collapsed_fold && folding_model_->IsLineHidden(line_index)) {
        wrapped_line_row_offsets_.push_back(last_visible_row);
        continue;
      }
      wrapped_line_row_offsets_.push_back(wrapped_row_layouts_.size());
      last_visible_row = wrapped_row_layouts_.size();
      const std::size_t line_visual_width =
          TextLayout::VisualColumnForTextColumn(document_->lines[line_index],
                                                document_->lines[line_index].size(), tab_size_);
      if (line_visual_width == 0) {
        wrapped_row_layouts_.push_back(WrappedRowLayout{line_index, 0, 0});
      } else {
        for (std::size_t start = 0; start < line_visual_width; start += wrap_columns) {
          const std::size_t end = std::min(line_visual_width, start + wrap_columns);
          wrapped_row_layouts_.push_back(WrappedRowLayout{line_index, start, end});
        }
      }
    }
  }
  if (wrapped_row_layouts_.empty()) {
    wrapped_row_layouts_.push_back(WrappedRowLayout{0, 0, 0});
    wrapped_line_row_offsets_.assign(document_->lines.size(), 0);
  }
  wrapped_row_layouts_tab_size_ = tab_size_;
  wrapped_row_layouts_visible_columns_ = visible_columns_;
  wrapped_row_layouts_revision_ = document_->layout_revision;
  wrapped_row_layouts_soft_wrap_ = soft_wrap_;
  wrapped_row_layouts_folding_model_ = folding_model_;
  wrapped_row_layouts_fold_revision_ =
      folding_model_ != nullptr ? folding_model_->revision() : 0;
#ifndef NDEBUG
  ++wrapped_row_layout_build_count_;
#endif
}

std::size_t TextViewport::CursorVisualRow() const {
  return CursorVisualRowForCaret(TextPosition{cursor_line_, cursor_column_});
}

std::size_t TextViewport::cursor_visual_row() const {
  return CursorVisualRow();
}

std::size_t TextViewport::PreferredColumnForCaret(const TextPosition& caret) const {
  if (caret.line >= document_->lines.size()) {
    return 0;
  }
  const std::size_t visual =
      TextLayout::VisualColumnForTextColumn(document_->lines[caret.line], caret.column, tab_size_);
  if (!soft_wrap_) {
    return visual;
  }
  EnsureWrappedRowLayouts();
  if (WrappedRowCount() == 0) {
    return 0;
  }
  const WrappedRowLayout row = WrappedRowAt(CursorVisualRowForCaret(caret));
  return visual >= row.visual_start ? visual - row.visual_start : 0;
}

std::size_t TextViewport::CursorVisualRowForCaret(const TextPosition& caret) const {
  EnsureWrappedRowLayouts();
  if (document_->lines.empty() || caret.line >= document_->lines.size()) {
    return 0;
  }
  // Trivial-layout path: identity mapping, no per-line offset vector to consult.
  if (wrapped_row_layouts_trivial_) {
    return caret.line;
  }
  if (wrapped_line_row_offsets_.size() != document_->lines.size()) {
    return 0;
  }
  const std::size_t base_row = wrapped_line_row_offsets_[caret.line];
  if (folding_model_ != nullptr && folding_model_->IsLineHidden(caret.line)) {
    return base_row;
  }
  if (!soft_wrap_) {
    return base_row;
  }
  const std::size_t wrap_columns = std::max<std::size_t>(1, visible_columns_);
  const std::size_t caret_visual =
      TextLayout::VisualColumnForTextColumn(document_->lines[caret.line], caret.column, tab_size_);
  const std::size_t row_in_line = caret_visual / wrap_columns;
  const std::size_t line_visual_width =
      TextLayout::VisualColumnForTextColumn(document_->lines[caret.line],
                                            document_->lines[caret.line].size(), tab_size_);
  const std::size_t rows_for_line = std::max<std::size_t>(1, (line_visual_width + wrap_columns - 1) /
                                                                 wrap_columns);
  const std::size_t clamped_in_line = std::min(row_in_line, rows_for_line - 1);
  return std::min(base_row + clamped_in_line,
                  wrapped_row_layouts_.empty() ? 0 : wrapped_row_layouts_.size() - 1);
}

std::size_t TextViewport::ResolveSoftWrapCursorColumnForTargetRow(std::size_t target_row) const {
  return ResolveSoftWrapCursorColumnForTargetRow(
      TextPosition{cursor_line_, cursor_column_}, preferred_column_, target_row);
}

std::size_t TextViewport::ResolveSoftWrapCursorColumnForTargetRow(
    const TextPosition& /*caret*/,
    std::size_t preferred_column,
    std::size_t target_row) const {
  EnsureWrappedRowLayouts();
  const std::size_t row_count = WrappedRowCount();
  if (row_count == 0) {
    return 0;
  }
  const std::size_t clamped_row = std::min(target_row, row_count - 1);
  const WrappedRowLayout target = WrappedRowAt(clamped_row);
  if (!soft_wrap_) {
    const std::size_t desired_absolute = horizontal_scroll_ + preferred_column;
    return desired_absolute;
  }
  if (target.visual_end <= target.visual_start) {
    return target.visual_start;
  }
  const std::size_t desired_absolute = target.visual_start + preferred_column;
  return std::min(desired_absolute, target.visual_end);
}

void TextViewport::AdvanceCaretHorizontal(TextPosition& caret, int delta) const {
  if (document_->lines.empty()) {
    return;
  }
  if (caret.line >= document_->lines.size()) {
    caret.line = document_->lines.size() - 1;
  }
  const std::string& line = document_->lines[caret.line];
  caret.column = std::min(caret.column, line.size());
  if (delta < 0) {
    for (int i = delta; i < 0; ++i) {
      caret.column = TextLayout::PreviousTextColumn(line, caret.column);
      if (caret.column == 0) {
        break;
      }
    }
  } else {
    for (int i = 0; i < delta; ++i) {
      caret.column = TextLayout::NextTextColumn(line, caret.column);
      if (caret.column >= line.size()) {
        break;
      }
    }
  }
}

void TextViewport::DedupeSecondaryCaretsAgainstPrimary() {
  std::sort(secondary_carets_.begin(), secondary_carets_.end(),
            [](const SecondaryCaret& lhs, const SecondaryCaret& rhs) {
              return TextPositionLess(lhs.position, rhs.position);
            });
  secondary_carets_.erase(
      std::unique(secondary_carets_.begin(), secondary_carets_.end(),
                  [](const SecondaryCaret& lhs, const SecondaryCaret& rhs) {
                    return lhs.position == rhs.position &&
                           lhs.selection_anchor == rhs.selection_anchor;
                  }),
      secondary_carets_.end());
  const TextPosition primary{cursor_line_, cursor_column_};
  secondary_carets_.erase(
      std::remove_if(secondary_carets_.begin(), secondary_carets_.end(),
                     [&](const SecondaryCaret& caret) { return caret.position == primary; }),
      secondary_carets_.end());
}

void TextViewport::AdvanceCaretVertical(TextPosition& caret,
                                        std::size_t& preferred_column,
                                        int delta) const {
  EnsureWrappedRowLayouts();
  const std::size_t row_count = WrappedRowCount();
  if (row_count == 0) {
    return;
  }
  const std::size_t current_row = CursorVisualRowForCaret(caret);
  const int max_row = static_cast<int>(row_count) - 1;
  const std::size_t target_row =
      static_cast<std::size_t>(std::clamp(static_cast<int>(current_row) + delta, 0, max_row));
  const WrappedRowLayout target = WrappedRowAt(target_row);
  const std::size_t target_visual_column =
      ResolveSoftWrapCursorColumnForTargetRow(caret, preferred_column, target_row);
  caret.line = target.line_index;
  caret.column = TextLayout::TextColumnForVisualColumn(document_->lines[target.line_index],
                                                       target_visual_column, tab_size_);
}

void TextViewport::EnsureDocument() {
  if (!document_) {
    document_ = std::make_shared<DocumentState>();
  }
}

TextViewport::TextEncoding TextViewport::DetectEncoding(std::string_view content) {
  if (content.find('\0') != std::string_view::npos) {
    return TextEncoding::Bytes;
  }

  const bool ascii_only = std::all_of(content.begin(), content.end(), [](char character) {
    return static_cast<unsigned char>(character) < 0x80;
  });
  if (ascii_only) {
    return TextEncoding::ASCII;
  }

  return util::IsValidUtf8(content) ? TextEncoding::UTF8 : TextEncoding::Bytes;
}

TextViewport::TextEncoding TextViewport::DetectEncoding(const std::vector<std::string>& lines) {
  bool ascii_only = true;
  for (const std::string& line : lines) {
    if (line.find('\0') != std::string::npos) {
      return TextEncoding::Bytes;
    }

    for (char character : line) {
      if (static_cast<unsigned char>(character) >= 0x80) {
        ascii_only = false;
        break;
      }
    }

    if (!ascii_only && !util::IsValidUtf8(line)) {
      return TextEncoding::Bytes;
    }
  }

  return ascii_only ? TextEncoding::ASCII : TextEncoding::UTF8;
}

std::vector<std::string> TextViewport::SliceLines(const std::vector<std::string>& lines,
                                                  std::size_t start_line,
                                                  std::size_t end_line) {
  const std::size_t clamped_start = std::min(start_line, lines.size());
  const std::size_t clamped_end = std::clamp(end_line, clamped_start, lines.size());
  return std::vector<std::string>(lines.begin() + static_cast<std::ptrdiff_t>(clamped_start),
                                  lines.begin() + static_cast<std::ptrdiff_t>(clamped_end));
}

SelectionRange TextViewport::NormalizeRange(const SelectionRange& range) {
  return IsBefore(range.start, range.end) ? range : SelectionRange{range.end, range.start};
}

bool TextViewport::IsBefore(const TextPosition& lhs, const TextPosition& rhs) {
  return lhs.line < rhs.line || (lhs.line == rhs.line && lhs.column < rhs.column);
}

}  // namespace microide::editor
