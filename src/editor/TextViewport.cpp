#include "editor/TextViewport.h"

#include <atomic>

#include "editor/SmartSelect.h"
#include "editor/PathKey.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/TextViewportInternal.h"
#include "editor/WordBoundary.h"

#include <algorithm>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/ScratchVector.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::editor {

std::uint64_t TextViewport::NextDocumentRevisionBase() {
  static std::atomic<std::uint64_t> documents{0};
  return (documents.fetch_add(1, std::memory_order_relaxed) + 1) << 32;
}

TextViewport::TextViewport() {
  document_ = std::make_shared<DocumentState>();
  SetPlaceholderText(
      "microide\n\n"
      "SDL3 shell scaffold is running.\n"
      "Open files from the sidebar with Enter.\n"
      "Ctrl+B toggles the sidebar, Ctrl+P opens the file finder.\n");
}

void TextViewport::SetLanguageOverride(std::string language) {
  EnsureDocument();
  if (document_->language_override == language) {
    return;
  }
  document_->language_override = std::move(language);
  ++document_->syntax_revision;
  language_id_valid_ = false;
}

const std::string& TextViewport::language_override() const {
  static const std::string kNone;
  return document_ != nullptr ? document_->language_override : kNone;
}

const std::string& TextViewport::language_id() const {
  util::AddPerformanceCounter(util::PerfCounterId::EditorFiletypeMemoQueries);
  if (document_ != nullptr && !document_->language_override.empty()) {
    static const std::string kFallbackLanguage = "unknown";
    return document_->language_override == runtime_syntax::kPlainTextFiletype ? kFallbackLanguage
                                                                              : document_->language_override;
  }
  const void* document = document_.get();
  const std::size_t registry_revision = runtime_syntax::RegistryRevision();
  const std::uint64_t revision =
      document == nullptr ? 0
      : language_id_content_sensitive_ ? document_->head_content_revision
                                       : 0;
  if (language_id_valid_ && language_id_document_ == document &&
      language_id_content_revision_ == revision &&
      language_id_registry_revision_ == registry_revision &&
      (document == nullptr || language_id_path_ == document_->path)) {
    util::AddPerformanceCounter(util::PerfCounterId::EditorFiletypeMemoHits);
    return language_id_;
  }
  bool content_consulted = false;
  if (document == nullptr) {
    language_id_.clear();
  } else {
    language_id_ = runtime_syntax::DetectFiletype(document_->path, LineSpan(document_->lines),
                                                  &content_consulted);
    language_id_path_ = document_->path;
  }
  language_id_document_ = document;
  language_id_content_sensitive_ = content_consulted;
  language_id_content_revision_ =
      content_consulted && document != nullptr ? document_->head_content_revision : 0;
  language_id_registry_revision_ = registry_revision;
  language_id_valid_ = true;
  return language_id_;
}

void TextViewport::SetPlaceholderText(std::string text) {
  EnsureDocument();
  ResetState(util::SplitLines(text), {}, LineEnding::LF, false, DetectEncoding(text), true, false);
}

void TextViewport::SetUntitledBuffer() {
  EnsureDocument();
  ResetState({""}, {}, LineEnding::LF, false, TextEncoding::ASCII, false, false);
}

TextViewport::LineCaret TextViewport::CaretForLine(std::size_t line_index) const {
  if (line_index != cursor_line_ || line_index >= document_->lines.size()) {
    return {};
  }
  const std::size_t caret_visual = VisualColumnAt(line_index, cursor_column_);
  if (caret_visual >= horizontal_scroll_ &&
      caret_visual <= horizontal_scroll_ + visible_columns_) {
    return LineCaret{true, caret_visual - horizontal_scroll_};
  }
  return {};
}

const LayoutLine& TextViewport::VisibleLineLayoutRef(std::size_t line_index) const {
  if (line_index >= document_->lines.size()) {
    static const LayoutLine kEmpty;
    return kEmpty;
  }
  return layout_cache_.VisibleLineLayoutRefCached(document_->lines, line_index, horizontal_scroll_,
                                                  visible_columns_, tab_size_,
                                                  document_->content_revision);
}

LayoutLine TextViewport::VisibleLineLayout(std::size_t line_index) const {
  if (line_index >= document_->lines.size()) {
    return LayoutLine{};
  }

  LayoutLine layout = VisibleLineLayoutRef(line_index);
  const LineCaret caret = CaretForLine(line_index);
  layout.caret_visible = caret.visible;
  layout.caret_column = caret.column;
  return layout;
}

TextViewport::WrappedRowLayout TextViewport::WrappedRowAt(std::size_t visual_row_index) const {
  return layout_cache_.WrappedRowAt(visual_row_index, horizontal_scroll_, visible_columns_);
}

std::size_t TextViewport::WrappedRowCount() const {
  return layout_cache_.WrappedRowCount(document_ != nullptr ? document_->lines.size() : 0);
}

LayoutLine TextViewport::VisibleWrappedRowLayout(std::size_t visual_row_index) const {
  if (!soft_wrap_) {
    return VisibleLineLayout(visual_row_index);
  }
  return VisibleWrappedRowLayout(visual_row_index, CursorVisualRow());
}

LayoutLine TextViewport::VisibleWrappedRowLayout(std::size_t visual_row_index,
                                                 std::size_t cursor_visual_row) const {
  if (!soft_wrap_) {
    return VisibleLineLayout(visual_row_index);
  }
  LayoutLine layout = VisibleWrappedRowLayoutRef(visual_row_index);
  const LineCaret caret = CaretForWrappedRow(visual_row_index, cursor_visual_row);
  layout.caret_visible = caret.visible;
  layout.caret_column = caret.column;
  return layout;
}

const LayoutLine& TextViewport::VisibleWrappedRowLayoutRef(std::size_t visual_row_index) const {
  static const LayoutLine kEmpty;
  if (!soft_wrap_) {
    return VisibleLineLayoutRef(visual_row_index);
  }
  EnsureWrappedRowLayouts();
  if (visual_row_index >= WrappedRowCount()) {
    return kEmpty;
  }
  const WrappedRowLayout row = WrappedRowAt(visual_row_index);
  if (row.line_index >= document_->lines.size()) {
    return kEmpty;
  }
  const std::size_t row_columns =
      row.visual_end > row.visual_start ? row.visual_end - row.visual_start : 0;
  return layout_cache_.VisibleLineLayoutRefCached(
      document_->lines, row.line_index, row.visual_start,
      std::min(visible_columns_, row_columns), tab_size_, document_->content_revision,
      LineLayoutFacts{
          .visual_columns = LineVisualWidthFromWrappedRows(row.line_index),
          .plain_ascii = false,
          .known = true,
      });
}

std::size_t TextViewport::LogicalLineVisualWidth(std::size_t line_index) const {
  if (document_ == nullptr || line_index >= document_->lines.size()) {
    return 0;
  }
  if (!soft_wrap_) {
    return VisibleLineLayoutRef(line_index).visual_columns;
  }
  EnsureWrappedRowLayouts();
  return LineVisualWidthFromWrappedRows(line_index);
}

std::size_t TextViewport::LineVisualWidthFromWrappedRows(std::size_t line_index) const {
  return WrappedRowAt(
             layout_cache_.WrappedRowRangeForLine(line_index, document_->lines.size()).second)
      .visual_end;
}

TextViewport::LineCaret TextViewport::CaretForWrappedRow(std::size_t visual_row_index,
                                                         std::size_t cursor_visual_row) const {
  if (!soft_wrap_) {
    return CaretForLine(visual_row_index);
  }
  if (visual_row_index != cursor_visual_row || visual_row_index >= WrappedRowCount()) {
    return {};
  }
  const WrappedRowLayout row = WrappedRowAt(visual_row_index);
  if (row.line_index != cursor_line_) {
    return {};
  }
  const std::size_t caret_visual = cursor_visual_column();
  return LineCaret{true,
                   caret_visual >= row.visual_start ? caret_visual - row.visual_start : 0};
}

TextViewport::WrappedVisualRow TextViewport::WrappedVisualRowLayout(std::size_t visual_row_index) const {
  EnsureWrappedRowLayouts();
  if (visual_row_index >= WrappedRowCount()) {
    return {};
  }
  return WrappedRowAt(visual_row_index);
}

LogicalPosition TextViewport::LogicalPositionForVisualHit(int visual_row, int visual_col) const {
  return ResolveVisualHit(visual_row, visual_col).position;
}

TextViewport::VisualHit TextViewport::ResolveVisualHit(int visual_row, int visual_col) const {
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
  const std::size_t hit_col = static_cast<std::size_t>(std::max(0, visual_col));
  const std::size_t local = hit_col > layout.indent ? hit_col - layout.indent : 0;
  const std::size_t clamped_local = std::min<std::size_t>(local, width);
  const std::size_t target_visual = layout.visual_start + clamped_local;
  return VisualHit{
      .position =
          LogicalPosition{
              .line = layout.line_index,
              .column = TextColumnAtVisualColumn(layout.line_index, target_visual),
          },
      .affinity = AffinityForRowLanding(clamped_row, target_visual),
  };
}

void TextViewport::MoveCursorToVisualHit(int visual_row, int visual_col, bool extend_selection) {
  if (document_->lines.empty()) {
    return;
  }
  const VisualHit hit = ResolveVisualHit(visual_row, visual_col);
  document_->undo_history.NotifyCursorMoved();
  BeginSelectionIfNeeded(extend_selection);
  const std::size_t clamped_line = std::min(hit.position.line, document_->lines.size() - 1);
  const std::size_t line_length = document_->lines.LineLength(clamped_line);
  const std::size_t clamped_column = TextLayout::ClampTextColumn(
      document_->lines.LineView(clamped_line), std::min(hit.position.column, line_length));
  PlacePrimaryCaret(clamped_line, clamped_column, false, hit.affinity);
  for (SecondaryCaret& caret : secondary_carets_) {
    caret.preferred_column = PreferredColumnForCaret(caret.position, caret.wrap_affinity);
  }
  EnsureCursorVisible();
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
  if (document_ == nullptr || document_->lines.empty()) {
    return 0;
  }
  if (layout_cache_.wrapped_row_layouts_trivial()) {
    return std::min<std::size_t>(line_index, document_->lines.size() - 1);
  }
  return layout_cache_.WrappedLineRowOffset(
      std::min<std::size_t>(line_index, document_->lines.size() - 1));
}

TextViewportCacheStats TextViewport::CacheStats() const {
  const TextLayoutCache::Stats layout_stats = layout_cache_.stats();
  return TextViewportCacheStats{
      .visible_line_queries = layout_stats.visible_line_queries,
      .visible_line_hits = layout_stats.visible_line_hits,
      .highlight_queries = highlight_queries_,
      .highlight_hits = highlight_hits_,
      .highlight_state_advances = highlight_state_advances_,
      .highlight_checkpoint_advances = highlight_checkpoint_advances_,
  };
}

void TextViewport::ResetCacheStats() const {
  layout_cache_.ResetStats();
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

std::vector<TextViewportUndoHistory::SecondaryCaret> TextViewport::secondary_caret_ranges() const {
  return secondary_carets_;
}

std::span<const TextPosition> TextViewport::secondary_caret_positions() const {
  const bool in_sync =
      secondary_caret_positions_cache_.size() == secondary_carets_.size() &&
      std::equal(secondary_carets_.begin(), secondary_carets_.end(),
                 secondary_caret_positions_cache_.begin(),
                 [](const SecondaryCaret& a, const TextPosition& b) { return a.position == b; });
  if (!in_sync) {
    secondary_caret_positions_cache_.clear();
    util::ReserveGrowing(secondary_caret_positions_cache_, secondary_carets_.size());
    for (const SecondaryCaret& caret : secondary_carets_) {
      secondary_caret_positions_cache_.push_back(caret.position);
    }
  }
  return secondary_caret_positions_cache_;
}

void TextViewport::AddSecondaryCaret(std::size_t line, std::size_t column) {
  SyncCaretsIfDocumentChangedElsewhere();

  if (document_->lines.empty()) {
    return;
  }
  const std::size_t clamped_line = std::min(line, document_->lines.size() - 1);
  const std::size_t clamped_column =
      TextLayout::ClampTextColumn(document_->lines.LineView(clamped_line), column);
  const TextPosition position{clamped_line, clamped_column};
  if (position == TextPosition{cursor_line_, cursor_column_}) {
    return;
  }
  const auto at = std::lower_bound(
      secondary_carets_.begin(), secondary_carets_.end(), position,
      [](const SecondaryCaret& caret, const TextPosition& value) {
        return detail::PositionLess(caret.position, value);
      });
  if (at != secondary_carets_.end() && at->position == position) {
    return;
  }
  secondary_carets_.insert(at, SecondaryCaret{
                                   .position = position,
                                   .preferred_column = PreferredColumnForCaret(position),
                                   .selection_anchor = std::nullopt,
                               });
  MergeOverlappingCaretRanges();
}

bool TextViewport::RemoveSecondaryCaretAt(TextPosition position) {
  const auto at = std::find_if(secondary_carets_.begin(), secondary_carets_.end(),
                               [&](const SecondaryCaret& caret) {
                                 return caret.position == position;
                               });
  if (at == secondary_carets_.end()) {
    return false;
  }
  secondary_carets_.erase(at);
  return true;
}

void TextViewport::AddSecondaryCaretWithRange(SelectionRange range) {
  SyncCaretsIfDocumentChangedElsewhere();

  if (document_->lines.empty()) {
    return;
  }
  const SelectionRange norm = NormalizeRange(range);
  if (!detail::ValidateRangeColumns(document_->lines, norm)) {
    return;
  }
  if (norm.start.line == norm.end.line && norm.start.column == norm.end.column) {
    AddSecondaryCaret(norm.start.line, norm.start.column);
    return;
  }
  TextPosition anchor = norm.start;
  TextPosition cursor_end = norm.end;
  if (!detail::PositionLess(anchor, cursor_end)) {
    std::swap(anchor, cursor_end);
  }
  const std::size_t clamped_line = std::min(cursor_end.line, document_->lines.size() - 1);
  cursor_end.column = TextLayout::ClampTextColumn(document_->lines.LineView(clamped_line), cursor_end.column);
  anchor.line = std::min(anchor.line, document_->lines.size() - 1);
  anchor.column = TextLayout::ClampTextColumn(document_->lines.LineView(anchor.line), anchor.column);

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
  DedupeSecondaryCaretsAgainstPrimary();
}

void TextViewport::SetSecondaryCarets(std::vector<TextPosition> carets) {
  secondary_carets_.clear();
  if (carets.empty() || document_->lines.empty()) {
    return;
  }
  const TextPosition primary{cursor_line_, cursor_column_};
  const std::size_t last_line = document_->lines.size() - 1;
  for (TextPosition& caret : carets) {
    caret.line = std::min(caret.line, last_line);
    caret.column = TextLayout::ClampTextColumn(document_->lines.LineView(caret.line), caret.column);
  }
  std::sort(carets.begin(), carets.end(), detail::PositionLess);
  secondary_carets_.reserve(carets.size());
  for (const TextPosition& caret : carets) {
    if (caret == primary) {
      continue;
    }
    if (!secondary_carets_.empty() && secondary_carets_.back().position == caret) {
      continue;
    }
    secondary_carets_.push_back(SecondaryCaret{
        .position = caret,
        .preferred_column = PreferredColumnForCaret(caret),
        .selection_anchor = std::nullopt,
    });
  }
}

void TextViewport::SetSecondaryCaretsWithRanges(std::span<const SelectionRange> ranges) {
  secondary_carets_.clear();
  if (ranges.empty() || document_->lines.empty()) {
    return;
  }
  const TextPosition primary{cursor_line_, cursor_column_};
  std::vector<SecondaryCaret>& candidates = secondary_caret_candidates_scratch_;
  candidates.clear();
  util::ReserveGrowing(candidates, ranges.size());
  for (const SelectionRange& range : ranges) {
    const SelectionRange norm = NormalizeRange(range);
    if (!detail::ValidateRangeColumns(document_->lines, norm)) {
      continue;
    }
    TextPosition anchor = range.start;
    TextPosition cursor_end = range.end;
    anchor.column = TextLayout::ClampTextColumn(document_->lines.LineView(anchor.line), anchor.column);
    cursor_end.column =
        TextLayout::ClampTextColumn(document_->lines.LineView(cursor_end.line), cursor_end.column);
    const bool empty_range = anchor == cursor_end;
    candidates.push_back(SecondaryCaret{
        .position = cursor_end,
        .preferred_column = PreferredColumnForCaret(cursor_end),
        .selection_anchor = empty_range ? std::nullopt : std::optional<TextPosition>(anchor),
    });
  }
  if (!std::is_sorted(candidates.begin(), candidates.end(), detail::SecondaryCaretPositionLess)) {
    std::sort(candidates.begin(), candidates.end(), detail::SecondaryCaretPositionLess);
  }
  util::ReserveGrowing(secondary_carets_, candidates.size());
  for (SecondaryCaret& candidate : candidates) {
    if (candidate.position == primary) {
      continue;
    }
    if (!secondary_carets_.empty() && secondary_carets_.back().position == candidate.position) {
      continue;
    }
    secondary_carets_.push_back(candidate);
  }
  MergeOverlappingCaretRanges();
}

void TextViewport::ClearSecondaryCarets() {
  secondary_carets_.clear();
}

void TextViewport::PruneCoincidentSecondaryCarets() {
  const TextPosition primary{cursor_line_, cursor_column_};
  detail::SortSecondaryCaretsIfNeeded(secondary_carets_);
  secondary_carets_.erase(
      std::unique(secondary_carets_.begin(), secondary_carets_.end(),
                  [](const SecondaryCaret& lhs, const SecondaryCaret& rhs) {
                    return lhs.position == rhs.position;
                  }),
      secondary_carets_.end());
  std::erase_if(secondary_carets_,
                [&](const SecondaryCaret& caret) { return caret.position == primary; });
}

void TextViewport::PlaceColumnCaretsBetweenLines(std::size_t anchor_line,
                                                 std::size_t target_line,
                                                 std::size_t column) {
  SyncCaretsIfDocumentChangedElsewhere();
  const std::size_t visual = BoxCornerVisualColumn(TextPosition{anchor_line, column});
  SetBoxSelectionVisual(anchor_line, visual, target_line, visual);
}

std::size_t TextViewport::MaxVisualWidthInSpan(std::size_t lo, std::size_t hi) const {
  if (document_->lines.empty()) {
    return 0;
  }
  const std::size_t last_line = document_->lines.size() - 1;
  lo = std::min(lo, last_line);
  hi = std::min(hi, last_line);
  if (lo > hi) {
    std::swap(lo, hi);
  }
  util::AddPerformanceCounter(util::PerfCounterId::EditorBoxSelectionSpanLinesScanned,
                              hi - lo + 1);
  std::size_t longest = 0;
  for (std::size_t line = lo; line <= hi; ++line) {
    const std::size_t line_length = document_->lines.LineLength(line);
    const LineLayoutFacts facts = CachedLineFacts(line);
    if (facts.known) {
      longest = std::max(longest, facts.visual_columns);
      continue;
    }
    const std::string_view text = document_->lines.LineView(line);
    const std::size_t tabs = util::LeadingByteRun(text, '\t');
    const std::size_t width =
        util::FirstNonAsciiOrByte(text.substr(tabs), '\t') == std::string_view::npos
            ? tabs * tab_size_ + (line_length - tabs)
            : TextLayout::VisualColumnForTextColumn(text, line_length, tab_size_);
    longest = std::max(longest, width);
  }
  return longest;
}

std::size_t TextViewport::BoxCornerVisualColumn(TextPosition corner) const {
  if (document_->lines.empty()) {
    return corner.column;
  }
  corner.line = std::min(corner.line, document_->lines.size() - 1);
  const std::size_t line_length = document_->lines.LineLength(corner.line);
  const std::size_t within = std::min(corner.column, line_length);
  return VisualColumnAt(corner.line, within) + (corner.column - within);
}

namespace {

std::size_t TabIndentedTextColumn(std::size_t visual_column, std::size_t tabs,
                                  std::size_t tab_size, std::size_t line_length) {
  const std::size_t indent_width = tabs * tab_size;
  if (visual_column >= indent_width) {
    return std::min(tabs + (visual_column - indent_width), line_length);
  }
  const std::size_t tab_index = visual_column / tab_size;
  const std::size_t into_tab = visual_column % tab_size;
  return into_tab * 2 <= tab_size ? tab_index : tab_index + 1;
}

}

void TextViewport::BoxColumnsOnLine(std::size_t line, std::size_t anchor_visual_column,
                                    std::size_t caret_visual_column, std::size_t* anchor_column,
                                    std::size_t* caret_column) {
  const LineLayoutFacts facts = CachedLineFacts(line);
  if (facts.known && facts.plain_ascii) {
    const std::size_t line_length = facts.visual_columns;
    *anchor_column = std::min(anchor_visual_column, line_length);
    *caret_column = std::min(caret_visual_column, line_length);
    return;
  }
  if (facts.known && facts.tab_indented) {
    const std::size_t line_length =
        facts.leading_tabs + (facts.visual_columns - facts.leading_tabs * tab_size_);
    *anchor_column =
        TabIndentedTextColumn(anchor_visual_column, facts.leading_tabs, tab_size_, line_length);
    *caret_column =
        TabIndentedTextColumn(caret_visual_column, facts.leading_tabs, tab_size_, line_length);
    return;
  }
  const std::string_view text = document_->lines.LineView(line);
  const std::size_t line_length = text.size();
  const std::size_t reach = std::min(std::max(anchor_visual_column, caret_visual_column), line_length);
  const std::size_t tabs = util::LeadingByteRun(text.substr(0, reach), '\t');
  if (util::FirstNonAsciiOrByte(text.substr(tabs, reach - tabs), '\t') == std::string_view::npos) {
    if (tabs == 0) {
      *anchor_column = std::min(anchor_visual_column, line_length);
      *caret_column = std::min(caret_visual_column, line_length);
    } else {
      *anchor_column = TabIndentedTextColumn(anchor_visual_column, tabs, tab_size_, line_length);
      *caret_column = TabIndentedTextColumn(caret_visual_column, tabs, tab_size_, line_length);
    }
    return;
  }
  *anchor_column = TextLayout::TextColumnForVisualColumn(text, anchor_visual_column, tab_size_);
  *caret_column = TextLayout::TextColumnForVisualColumn(text, caret_visual_column, tab_size_);
}

void TextViewport::SetBoxSelection(TextPosition anchor, TextPosition caret) {
  SyncCaretsIfDocumentChangedElsewhere();

  if (document_->lines.empty()) {
    return;
  }
  SetBoxSelectionVisual(anchor.line, BoxCornerVisualColumn(anchor), caret.line,
                        BoxCornerVisualColumn(caret));
}

void TextViewport::SetBoxSelectionVisual(std::size_t anchor_line,
                                         std::size_t anchor_visual_column,
                                         std::size_t caret_line,
                                         std::size_t caret_visual_column) {
  SyncCaretsIfDocumentChangedElsewhere();
  util::PerformanceTrace::Scope trace_scope("TextViewport::SetBoxSelection");
  if (document_->lines.empty()) {
    return;
  }
  util::AddPerformanceCounter(util::PerfCounterId::EditorBoxSelectionBuilds);
  const std::size_t last_line = document_->lines.size() - 1;
  anchor_line = std::min(anchor_line, last_line);
  caret_line = std::min(caret_line, last_line);

  std::size_t lo = std::min(anchor_line, caret_line);
  std::size_t hi = std::max(anchor_line, caret_line);

  constexpr std::size_t kMaxColumnCarets = 10000;
  if (hi - lo + 1 > kMaxColumnCarets) {
    if (caret_line >= hi) {
      lo = hi - (kMaxColumnCarets - 1);
    } else {
      hi = lo + (kMaxColumnCarets - 1);
    }
  }

  ClearSecondaryCarets();

  util::AddPerformanceCounter(util::PerfCounterId::EditorBoxSelectionCaretsPlaced, hi - lo + 1);
  std::vector<SelectionRange>& ranges = box_ranges_scratch_;
  ranges.clear();
  util::ReserveGrowing(ranges, hi - lo);
  for (std::size_t line = lo; line <= hi; ++line) {
    if (line == caret_line) {
      continue;
    }
    std::size_t a = 0;
    std::size_t c = 0;
    BoxColumnsOnLine(line, anchor_visual_column, caret_visual_column, &a, &c);
    ranges.push_back(SelectionRange{TextPosition{line, a}, TextPosition{line, c}});
  }

  std::size_t primary_anchor = 0;
  std::size_t primary_caret = 0;
  BoxColumnsOnLine(caret_line, anchor_visual_column, caret_visual_column, &primary_anchor,
                   &primary_caret);
  MoveCursorTo(caret_line, primary_anchor, false);
  MoveCursorTo(caret_line, primary_caret, true);
  SetSecondaryCaretsWithRanges(ranges);
}

bool TextViewport::has_selection() const {
  return selection_range().has_value();
}

std::optional<SelectionRange> TextViewport::selection_range() const {
  if (!selection_anchor_.has_value()) {
    return std::nullopt;
  }

  TextPosition cursor{cursor_line_, cursor_column_};
  TextPosition anchor = *selection_anchor_;
  if (CaretsLagDocument()) {
    cursor = ClampedToDocument(cursor);
    anchor = ClampedToDocument(anchor);
  }
  if (anchor.line == cursor.line && anchor.column == cursor.column) {
    return std::nullopt;
  }

  if (IsBefore(anchor, cursor)) {
    return SelectionRange{anchor, cursor};
  }
  return SelectionRange{cursor, anchor};
}

std::string TextViewport::TextInRange(const SelectionRange& range) const {
  const auto& start = range.start;
  const auto& end = range.end;
  if (start.line == end.line) {
    return std::string(
        document_->lines.LineView(start.line).substr(start.column, end.column - start.column));
  }

  std::size_t total_bytes = document_->lines.LineLength(start.line) - start.column;
  for (std::size_t line = start.line + 1; line < end.line; ++line) {
    total_bytes += 1 + document_->lines.LineLength(line);
  }
  total_bytes += 1 + end.column;

  std::string text;
  text.reserve(total_bytes);
  text += document_->lines.LineView(start.line).substr(start.column);
  text.push_back('\n');
  for (std::size_t line = start.line + 1; line < end.line; ++line) {
    text += document_->lines.LineView(line);
    text.push_back('\n');
  }
  text += document_->lines.LineView(end.line).substr(0, end.column);
  return text;
}

std::string TextViewport::SelectedText() const {
  const auto range = selection_range();
  if (!range.has_value()) {
    return {};
  }
  return TextInRange(*range);
}

std::size_t TextViewport::CollapsedFoldEndAt(std::size_t line) const {
  if (folding_model_ == nullptr || !folding_model_->has_any_collapsed_fold() ||
      document_->lines.empty()) {
    return line;
  }
  for (const FoldRange& range : folding_model_->collapsed_ranges()) {
    if (range.opener_line == line) {
      return std::max(line, std::min(range.closer_line, document_->lines.size() - 1));
    }
  }
  return line;
}

std::size_t TextViewport::VisibleLineAfter(std::size_t line) const {
  const std::size_t line_count = document_->lines.size();
  if (line + 1 >= line_count) {
    return line;
  }
  if (folding_model_ == nullptr || !folding_model_->has_any_collapsed_fold()) {
    return line + 1;
  }
  const std::size_t next = folding_model_->FirstVisibleLineAtOrAfter(line + 1, line_count);
  return next >= line_count ? line : next;
}

std::size_t TextViewport::VisibleLineBefore(std::size_t line) const {
  if (line == 0 || document_->lines.empty()) {
    return line;
  }
  if (folding_model_ == nullptr || !folding_model_->has_any_collapsed_fold()) {
    return line - 1;
  }
  const std::size_t previous = folding_model_->LastVisibleLineAtOrBefore(line - 1);
  return previous == FoldingModel::kNoVisibleLine ? line : previous;
}

void TextViewport::AppendCaretLineVerbLines(std::vector<std::size_t>* out) const {
  if (out == nullptr || document_->lines.empty()) {
    return;
  }
  const std::size_t last_line = document_->lines.size() - 1;
  const auto append_block = [&](std::size_t line) {
    const std::size_t start = std::min(line, last_line);
    const std::size_t end = std::min(CollapsedFoldEndAt(start), last_line);
    for (std::size_t i = start; i <= end; ++i) {
      out->push_back(i);
    }
  };
  append_block(cursor_line_);
  for (const SecondaryCaret& caret : secondary_carets_) {
    append_block(caret.position.line);
  }
  std::sort(out->begin(), out->end());
  out->erase(std::unique(out->begin(), out->end()), out->end());
}

std::optional<std::string> TextViewport::MultiCaretLineTextForClipboard() const {
  if (!has_multiple_carets() || document_->lines.empty()) {
    return std::nullopt;
  }
  std::vector<std::size_t> lines;
  lines.reserve(secondary_carets_.size() + 1);
  AppendCaretLineVerbLines(&lines);
  if (lines.empty()) {
    return std::nullopt;
  }
  std::string text;
  std::size_t bytes = 0;
  for (const std::size_t line : lines) {
    bytes += document_->lines.LineLength(line) + 1;
  }
  text.reserve(bytes);
  for (const std::size_t line : lines) {
    text += document_->lines.LineView(line);
    text.push_back('\n');
  }
  return text;
}

std::string TextViewport::CurrentLineTextForClipboard() const {
  if (document_->lines.empty()) {
    return {};
  }

  const std::size_t last = CollapsedFoldEndAt(cursor_line_);
  std::string text;
  text.reserve(document_->lines.LineLength(cursor_line_) + 1);
  for (std::size_t line = cursor_line_; line <= last; ++line) {
    text += document_->lines.LineView(line);
    text.push_back('\n');
  }
  return text;
}

bool TextViewport::DeleteSelectedText() {
  return DeleteSelection();
}

bool TextViewport::DeleteCurrentLine() {
  SyncCaretsIfDocumentChangedElsewhere();

  if (document_->lines.empty()) {
    return false;
  }
  if (has_multiple_carets()) {
    std::vector<std::size_t> lines_to_delete;
    lines_to_delete.reserve(secondary_carets_.size() + 1);
    AppendCaretLineVerbLines(&lines_to_delete);
    if (lines_to_delete.empty()) {
      return false;
    }

    const std::size_t before_document_line_count = document_->lines.size();
    const std::size_t before_lines_start = lines_to_delete.front();
    const std::size_t before_lines_end = lines_to_delete.back() + 1;
    LineBlob before_lines =
        document_->lines.SliceLinesBlob(before_lines_start, before_lines_end);
    const ViewState before_state = CaptureViewState();
    for (auto it = lines_to_delete.rbegin(); it != lines_to_delete.rend(); ++it) {
      document_->lines.EraseLine(*it);
    }
    if (document_->lines.empty()) {
      document_->lines.PushBackLine("");
    }
    cursor_line_ = std::min(cursor_line_, document_->lines.size() - 1);
    cursor_column_ = 0;
    preferred_column_ = 0;
    selection_anchor_.reset();
    secondary_carets_.clear();
    document_->placeholder = false;
    document_->dirty = true;
    RefreshEncoding();
    InvalidateDerivedCaches(InvalidationReason::ContentEdit, 0);
    InvalidateVisualColumnCache();
    EnsureCursorVisible();
    const std::ptrdiff_t line_delta = static_cast<std::ptrdiff_t>(document_->lines.size()) -
                                      static_cast<std::ptrdiff_t>(before_document_line_count);
    const std::size_t after_slice_size =
        static_cast<std::size_t>(std::max<std::ptrdiff_t>(
            0, static_cast<std::ptrdiff_t>(before_lines.size()) + line_delta));
    const std::size_t after_lines_start = std::min(before_lines_start, document_->lines.size());
    const std::size_t after_lines_end =
        std::min(document_->lines.size(), before_lines_start + after_slice_size);
    HistoryEntry aggregate_entry = TextViewportUndoHistory::BuildEntryForDocumentChange(
        std::move(before_lines), before_state,
        document_->lines.SliceLinesBlob(after_lines_start, after_lines_end), CaptureViewState());
    aggregate_entry.start_line += before_lines_start;
    ClearLastAppliedEdit();
    PushHistoryEntry(std::move(aggregate_entry));
    return true;
  }

  const std::size_t last = CollapsedFoldEndAt(cursor_line_);
  if (cursor_line_ == 0 && last + 1 == document_->lines.size()) {
    return ApplyRangeEdit(SelectionRange{
                              .start = TextPosition{0, 0},
                              .end = TextPosition{last, document_->lines.LineLength(last)},
                          },
                          "", true);
  }

  if (last + 1 < document_->lines.size()) {
    return ApplyRangeEdit(SelectionRange{
                              .start = TextPosition{cursor_line_, 0},
                              .end = TextPosition{last + 1, 0},
                          },
                          "", true);
  }

  const std::size_t previous_line = cursor_line_ - 1;
  return ApplyRangeEdit(SelectionRange{
                            .start = TextPosition{
                                previous_line,
                                document_->lines.LineLength(previous_line),
                            },
                            .end = TextPosition{last, document_->lines.LineLength(last)},
                        },
                        "", true);
}

void TextViewport::ClearSelection() {
  selection_anchor_.reset();
}

void TextViewport::SelectAll() {
  SyncCaretsIfDocumentChangedElsewhere();

  if (document_->lines.empty()) {
    return;
  }
  ClearSecondaryCarets();

  selection_anchor_ = TextPosition{0, 0};
  cursor_line_ = document_->lines.size() - 1;
  cursor_column_ = document_->lines.LineLength(cursor_line_);
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  EnsureCursorVisible();
}

std::optional<SelectionRange> TextViewport::WordRangeAt(TextPosition position) const {
  if (document_->lines.empty() || position.line >= document_->lines.size()) {
    return std::nullopt;
  }
  const std::string_view line = document_->lines.LineView(position.line);
  const std::size_t col = std::min(position.column, line.size());
  const WordSpan span = IdentifierRunTouching(line, col);
  if (span.empty()) {
    return std::nullopt;
  }
  return SelectionRange{TextPosition{position.line, span.start}, TextPosition{position.line, span.end}};
}

SelectionRange TextViewport::LineRangeAt(std::size_t line_index) const {
  if (document_->lines.empty()) {
    return SelectionRange{TextPosition{0, 0}, TextPosition{0, 0}};
  }
  const std::size_t line = std::min(line_index, document_->lines.size() - 1);
  if (line + 1 < document_->lines.size()) {
    return SelectionRange{TextPosition{line, 0}, TextPosition{line + 1, 0}};
  }
  return SelectionRange{TextPosition{line, 0},
                        TextPosition{line, document_->lines.LineLength(line)}};
}

void TextViewport::SelectWordAtCursor() {
  SyncCaretsIfDocumentChangedElsewhere();

  const auto word = WordRangeAt(TextPosition{cursor_line_, cursor_column_});
  if (!word.has_value()) {
    return;
  }
  SelectOnCursorLine(word->start.column, word->end.column);
}

std::optional<SelectionRange> TextViewport::WordSelectionRangeAt(TextPosition position) const {
  if (document_->lines.empty() || position.line >= document_->lines.size()) {
    return std::nullopt;
  }
  const std::string_view line = document_->lines.LineView(position.line);
  const WordSpan span = WordSelectionRunAt(line, std::min(position.column, line.size()));
  if (span.empty()) {
    return std::nullopt;
  }
  return SelectionRange{TextPosition{position.line, span.start},
                        TextPosition{position.line, span.end}};
}

void TextViewport::SelectWordOrRunAtCursor() {
  const auto range = WordSelectionRangeAt(TextPosition{cursor_line_, cursor_column_});
  if (!range.has_value()) {
    return;
  }
  SelectOnCursorLine(range->start.column, range->end.column);
}

void TextViewport::SelectOnCursorLine(std::size_t start_column, std::size_t end_column) {
  selection_anchor_ = TextPosition{cursor_line_, start_column};
  cursor_column_ = end_column;
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  EnsureCursorVisible();
}

std::optional<SelectionRange> TextViewport::OccurrenceSeedSpanForHighlight() const {
  if (document_->lines.empty()) {
    return std::nullopt;
  }


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
  const std::string_view line = document_->lines.LineView(line_index);
  const std::size_t col = std::min(cursor_column_, line.size());
  const WordSpan span = IdentifierRunTouching(line, col);
  if (span.empty()) {
    return std::nullopt;
  }
  return SelectionRange{{line_index, span.start}, {line_index, span.end}};
}

void TextViewport::SelectLineAtCursor() {
  SyncCaretsIfDocumentChangedElsewhere();

  if (document_->lines.empty()) {
    return;
  }
  const SelectionRange range = LineRangeAt(cursor_line_);
  selection_anchor_ = range.start;
  cursor_line_ = range.end.line;
  cursor_column_ = range.end.column;
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  EnsureCursorVisible();
}

bool TextViewport::ExpandSelection() {
  SyncCaretsIfDocumentChangedElsewhere();
  const TextPosition caret{cursor_line_, cursor_column_};
  const SelectionRange current = selection_range().value_or(SelectionRange{caret, caret});
  const std::optional<SelectionRange> next = ExpandSelectionRange(document_->lines, current);
  if (!next.has_value()) {
    return false;
  }
  const bool continues = !expand_selection_history_.empty() &&
                         expand_selection_history_.back().start == current.start &&
                         expand_selection_history_.back().end == current.end;
  if (!continues) {
    expand_selection_history_.assign(1, current);
  }
  expand_selection_history_.push_back(*next);
  SelectRangeForExpansion(*next);
  return true;
}

bool TextViewport::ShrinkSelection() {
  SyncCaretsIfDocumentChangedElsewhere();
  const TextPosition caret{cursor_line_, cursor_column_};
  const SelectionRange current = selection_range().value_or(SelectionRange{caret, caret});
  if (expand_selection_history_.size() < 2 || !(expand_selection_history_.back().start == current.start) ||
      !(expand_selection_history_.back().end == current.end)) {
    expand_selection_history_.clear();
    return false;
  }
  expand_selection_history_.pop_back();
  SelectRangeForExpansion(expand_selection_history_.back());
  if (expand_selection_history_.size() == 1) {
    expand_selection_history_.clear();
  }
  return true;
}

void TextViewport::SelectRangeForExpansion(const SelectionRange& range) {
  if (range.start == range.end) {
    selection_anchor_.reset();
  } else {
    selection_anchor_ = range.start;
  }
  cursor_line_ = range.end.line;
  cursor_column_ = range.end.column;
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  EnsureCursorVisible();
}

void TextViewport::SetDocumentPath(const std::filesystem::path& path) {
  document_->path = path;
  document_->path_key = NormalizedPathKey(path);
  document_->real_path.clear();
  if (!path.empty()) {
    if (std::filesystem::path real = util::ResolveRealPath(path); real != path) {
      document_->real_path = std::move(real);
    }
  }
}

void TextViewport::ResetState(std::vector<std::string> lines,
                              const std::filesystem::path& path,
                              LineEnding line_ending,
                              bool mixed_line_endings,
                              TextEncoding encoding,
                              bool placeholder,
                              bool dirty) {
  EnsureDocument();
  if (lines.empty()) {
    document_->lines.Reset(std::vector<std::string>{""});
  } else {
    document_->lines.Reset(lines);
  }
  ResetMetadataAfterContent(path, line_ending, mixed_line_endings, encoding, placeholder, dirty);
}

void TextViewport::ResetStateFromText(std::string text,
                                      const std::filesystem::path& path,
                                      LineEnding line_ending,
                                      bool mixed_line_endings,
                                      TextEncoding encoding,
                                      bool placeholder,
                                      bool dirty) {
  EnsureDocument();
  document_->lines.ResetFromText(std::move(text));
  ResetMetadataAfterContent(path, line_ending, mixed_line_endings, encoding, placeholder, dirty);
}

void TextViewport::ResetMetadataAfterContent(const std::filesystem::path& path,
                                             LineEnding line_ending,
                                             bool mixed_line_endings,
                                             TextEncoding encoding,
                                             bool placeholder,
                                             bool dirty) {
  SetDocumentPath(path);
  document_->line_ending = line_ending;
  document_->mixed_line_endings = mixed_line_endings;
  document_->encoding = encoding;
  document_->utf8_bom = false;
  cursor_line_ = 0;
  cursor_column_ = 0;
  preferred_column_ = 0;
  scroll_line_ = 0;
  horizontal_scroll_ = 0;
  selection_anchor_.reset();
  secondary_carets_.clear();
  document_->undo_history.Clear();
  document_->placeholder = placeholder;
  document_->dirty = dirty;
  document_->disk_signature = util::StatFileSignature(path);
  InvalidateVisualColumnCache();
  InvalidateDerivedCaches(InvalidationReason::ContentEdit, 0);
  EnsureCursorVisible();
}

void TextViewport::InvalidateDerivedCaches(InvalidationReason reason) {
  InvalidateDerivedCaches(reason, 0);
}

void TextViewport::InvalidateDerivedCaches(InvalidationReason reason, std::size_t start_line,
                                           std::optional<ContentSplice> splice) {
  util::PerformanceTrace::Scope perf_scope("TextViewport::InvalidateDerivedCaches");
  EnsureDocument();
  ++document_->presentation_revision;
  util::AddPerformanceCounter(util::PerfCounterId::EditorPresentationRevisionBumps);
  if (reason == InvalidationReason::ContentEdit) {
    ++document_->content_revision;
    carets_synced_content_revision_ = document_->content_revision;
    if (start_line < runtime_syntax::kFiletypeDetectHeadLines) {
      ++document_->head_content_revision;
    }
    util::AddPerformanceCounter(util::PerfCounterId::EditorContentRevisionBumps);
  } else if (reason == InvalidationReason::SyntaxConfig) {
    ++document_->syntax_revision;
    util::AddPerformanceCounter(util::PerfCounterId::EditorSyntaxRevisionBumps);
  } else if (reason == InvalidationReason::LayoutShape) {
    ++document_->layout_shape_revision;
    util::AddPerformanceCounter(util::PerfCounterId::EditorLayoutShapeRevisionBumps);
  }

  if (reason != InvalidationReason::ContentEdit) {
    if (reason == InvalidationReason::SyntaxConfig) {
      DropHighlightTokenCache();
      initial_highlight_state_.reset();
      line_highlight_states_.clear();
      line_highlight_states_valid_through_ = 0;
      highlight_checkpoints_.clear();
      highlight_checkpoints_valid_through_ = 0;
      highlight_state_content_revision_ = document_->content_revision;
      highlight_state_syntax_revision_ = document_->syntax_revision;
    }
    return;
  }

  const std::size_t safe_start = std::min(start_line, document_->lines.size());
  edit_carry_before_.reset();
  if (splice.has_value() && splice->removed == splice->inserted && splice->inserted > 0) {
    const std::size_t last = safe_start + splice->inserted - 1;
    if (last < line_highlight_states_valid_through_ && last < line_highlight_states_.size() &&
        highlight_state_content_revision_ + 1 == document_->content_revision &&
        highlight_state_syntax_revision_ == document_->syntax_revision) {
      edit_carry_before_ = EditCarry{.first_line = safe_start,
                                     .line = last,
                                     .before = line_highlight_states_[last],
                                     .states_valid_through_before = line_highlight_states_valid_through_,
                                     .checkpoints_valid_through_before = highlight_checkpoints_valid_through_,
                                     .content_revision = document_->content_revision};
    }
  }
  util::AddPerformanceCounter(util::PerfCounterId::EditorInvalidateDerivedCachesCalls);
  util::AddPerformanceCounter(util::PerfCounterId::EditorInvalidateDerivedCachesLines,
                              document_->lines.size() - safe_start);

  if (splice.has_value()) {
    fold_edit_span_.NoteSplice(safe_start, splice->removed, splice->inserted);
  } else {
    fold_edit_span_.NoteSuffixReplaced(safe_start);
  }
  layout_cache_.CarryPlainPrefixMemosAcrossEdit(
      LineSpan(document_->lines), safe_start,
      splice.has_value() ? splice->edit_column : TextLayoutCache::kNoEditColumn,
      document_->content_revision - 1, document_->content_revision);

  if (safe_start == 0) {
    layout_cache_.InvalidateVisibleLineCacheFrom(0, document_->content_revision);
    DropHighlightTokenCache();
    initial_highlight_state_.reset();
    line_highlight_states_valid_through_ = 0;
    highlight_checkpoints_valid_through_ = 0;
    highlight_state_content_revision_ = document_->content_revision;
    highlight_state_syntax_revision_ = document_->syntax_revision;
    return;
  }

  layout_cache_.InvalidateVisibleLineCacheFrom(safe_start, document_->content_revision);

  StaleHighlightTokensFrom(safe_start);

  line_highlight_states_valid_through_ =
      std::min(line_highlight_states_valid_through_, safe_start);

  const std::size_t checkpoint_count =
      document_->lines.empty() ? 0
                               : ((document_->lines.size() - 1) / detail::kHighlightCheckpointInterval) + 1;
  if (highlight_checkpoints_.size() != checkpoint_count) {
    highlight_checkpoints_.resize(checkpoint_count);
  }
  const std::size_t checkpoint_start = safe_start / detail::kHighlightCheckpointInterval;
  highlight_checkpoints_valid_through_ =
      std::min(highlight_checkpoints_valid_through_, checkpoint_start);
  if (!highlight_checkpoints_.empty()) {
    EnsureInitialHighlightState();
    if (initial_highlight_state_.has_value()) {
      highlight_checkpoints_.front() = *initial_highlight_state_;
      if (highlight_checkpoints_valid_through_ < 1) {
        highlight_checkpoints_valid_through_ = 1;
      }
    } else {
      highlight_checkpoints_valid_through_ = 0;
    }
  }
  highlight_state_content_revision_ = document_->content_revision;
  highlight_state_syntax_revision_ = document_->syntax_revision;
}

LineEditSpan TextViewport::ConsumeFoldEditSpan() {
  const LineEditSpan span = fold_edit_span_;
  fold_edit_span_.Clear();
  return span;
}

void TextViewport::InvalidateVisualColumnCache() { layout_cache_.InvalidateAll(); }

void TextViewport::InvalidateLayoutCaches() {
  InvalidateVisualColumnCache();
  InvalidateDerivedCaches(InvalidationReason::ContentEdit, 0);
}

void TextViewport::InvalidateSyntaxHighlighting() {
  InvalidateDerivedCaches(InvalidationReason::SyntaxConfig, 0);
}

void TextViewport::BeginSelectionIfNeeded(bool extend_selection) {
  if (extend_selection) {
    if (!selection_anchor_.has_value()) {
      selection_anchor_ = TextPosition{cursor_line_, cursor_column_};
    }
    for (SecondaryCaret& caret : secondary_carets_) {
      if (!caret.selection_anchor.has_value()) {
        caret.selection_anchor = caret.position;
      }
    }
    return;
  }
  selection_anchor_.reset();
  for (SecondaryCaret& caret : secondary_carets_) {
    caret.selection_anchor.reset();
  }
}

void TextViewport::UpdateVisualColumnCacheAfterEdit(std::size_t start_line,
                                                    std::size_t removed_count,
                                                    std::size_t inserted_count,
                                                    InlineLineSplice splice) {
  util::PerformanceTrace::Scope perf_scope("TextViewport::UpdateVisualColumnCacheAfterEdit");
  layout_cache_.UpdateVisualColumnCacheAfterEdit(start_line, removed_count, inserted_count,
                                                  document_->lines, tab_size_,
                                                  document_->content_revision, splice);
}

void TextViewport::UpdateWrappedRowsAfterEdit(std::size_t start_line,
                                              std::size_t removed_count,
                                              std::size_t inserted_count,
                                              std::size_t edit_column) {
  util::PerformanceTrace::Scope perf_scope("TextViewport::UpdateWrappedRowsAfterEdit");
  layout_cache_.UpdateWrappedRowsAfterEdit(start_line, removed_count, inserted_count,
                                           document_->lines, tab_size_, visible_columns_,
                                           soft_wrap_, folding_model_,
                                           document_->layout_shape_revision,
                                           document_->content_revision, edit_column);
}

std::size_t TextViewport::MaxVisualColumns() const {
  return layout_cache_.MaxVisualColumns(document_->lines, tab_size_, document_->content_revision);
}

std::string TextViewport::VerifyDerivedCachesForTesting() const {
  if (!document_) {
    return {};
  }
  return layout_cache_.VerifyAgainstFreshBuildForTesting(document_->lines, tab_size_, visible_columns_, soft_wrap_,
                                                         folding_model_, document_->layout_shape_revision,
                                                         document_->content_revision);
}

void TextViewport::EnsureWrappedRowLayouts() const {
  if (!document_) {
    return;
  }
  layout_cache_.EnsureWrappedRowLayouts(document_->lines, tab_size_, visible_columns_, soft_wrap_,
                                        folding_model_, document_->layout_shape_revision,
                                        document_->content_revision);
}

SelectionRange TextViewport::NormalizeRange(const SelectionRange& range) {
  return IsBefore(range.start, range.end) ? range : SelectionRange{range.end, range.start};
}

bool TextViewport::IsBefore(const TextPosition& lhs, const TextPosition& rhs) {
  return detail::PositionLess(lhs, rhs);
}

}
