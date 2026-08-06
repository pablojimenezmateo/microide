#include "editor/TextViewport.h"
#include "editor/PathKey.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/TextViewportInternal.h"

#include <algorithm>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/ScratchVector.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

namespace microide::editor {

// `kHighlightCheckpointInterval` is declared in editor/TextViewportInternal.h
// (detail namespace) so the highlight-cache sibling TU sees the same value.
// `PositionLess`, `SelectionRangeForSecondaryCaret`, `RangeEndExclusive`,
// `ValidateRangeColumns`, and `TextBetweenLines` live in
// `editor/TextViewportInternal.h` so they are shared with the language-behavior
// sibling translation unit. Use them via `detail::`.

TextViewport::TextViewport() {
  document_ = std::make_shared<DocumentState>();
  SetPlaceholderText(
      "microide\n\n"
      "SDL3 shell scaffold is running.\n"
      "Open files from the sidebar with Enter.\n"
      "Ctrl+B toggles the sidebar, Ctrl+P opens the file finder.\n");
}

TextViewport::TextViewport(const TextViewport& other)
    : fold_edit_span_(other.fold_edit_span_),
      document_(other.document_),
      cursor_line_(other.cursor_line_),
      cursor_column_(other.cursor_column_),
      preferred_column_(other.preferred_column_),
      caret_navigation_content_revision_(other.caret_navigation_content_revision_),
      scroll_line_(other.scroll_line_),
      horizontal_scroll_(other.horizontal_scroll_),
      visible_lines_(other.visible_lines_),
      visible_columns_(other.visible_columns_),
      tab_size_(other.tab_size_),
      indent_width_(other.indent_width_),
      soft_tabs_(other.soft_tabs_),
      soft_wrap_(other.soft_wrap_),
      save_trim_trailing_whitespace_(other.save_trim_trailing_whitespace_),
      save_ensure_final_newline_(other.save_ensure_final_newline_),
      save_line_ending_override_(other.save_line_ending_override_),
      lc_view_(other.lc_view_),
      language_id_(other.language_id_),
      language_id_document_(other.language_id_document_),
      language_id_path_(other.language_id_path_),
      language_id_content_revision_(other.language_id_content_revision_),
      language_id_registry_revision_(other.language_id_registry_revision_),
      language_id_valid_(other.language_id_valid_),
      secondary_carets_(other.secondary_carets_),
      column_selection_(other.column_selection_),
      secondary_caret_positions_cache_(other.secondary_caret_positions_cache_),
      // box_ranges_scratch_ and secondary_caret_candidates_scratch_ are deliberately
      // left empty rather than copied. They hold no state between calls — every use
      // clears them on entry — so copying their contents would allocate to carry
      // bytes the next call throws away. The capacity rebuilds on first use.
      box_ranges_scratch_(),
      secondary_caret_candidates_scratch_(),
      layout_cache_(other.layout_cache_),
      highlight_cache_(other.highlight_cache_),
      highlight_cache_order_(other.highlight_cache_order_),
      initial_highlight_state_(other.initial_highlight_state_),
      line_highlight_states_(other.line_highlight_states_),
      line_highlight_states_valid_through_(other.line_highlight_states_valid_through_),
      highlight_checkpoints_(other.highlight_checkpoints_),
      highlight_checkpoints_valid_through_(other.highlight_checkpoints_valid_through_),
      pending_checkpoint_backfill_target_line_(other.pending_checkpoint_backfill_target_line_),
      last_highlight_state_exact_(other.last_highlight_state_exact_),
      highlight_state_content_revision_(other.highlight_state_content_revision_),
      highlight_state_syntax_revision_(other.highlight_state_syntax_revision_),
      highlight_queries_(other.highlight_queries_),
      highlight_hits_(other.highlight_hits_),
      highlight_state_advances_(other.highlight_state_advances_),
      highlight_checkpoint_advances_(other.highlight_checkpoint_advances_),
      selection_anchor_(other.selection_anchor_),
      last_applied_edit_(other.last_applied_edit_),
      last_applied_edit_line_span_(other.last_applied_edit_line_span_),
      folding_model_(nullptr),
      undo_history_(other.undo_history_) {
  // Drop ONLY the fold-dependent half of the layout cache. This used to be a
  // full InvalidateVisualColumnCache(), which threw away the per-line width
  // table the copy had just deep-copied -- so every viewport copy handed the new
  // viewport an empty table that the next ClampScrollState() rebuilt with an
  // O(document) walk. Reloading an already-open buffer copies a viewport twice,
  // which is how a 50k-line file measured exactly two whole-document width
  // rebuilds per open (TD-2026-08-06-138).
  //
  // Widths are a function of the document's bytes and the tab size; the copy
  // shares `document_` and `tab_size_`, so the table and the visible-line LRU
  // keyed on them are still exactly right. Only `folding_model_` is reset here,
  // and the one product that depends on it is the wrapped-row table. Same
  // reasoning as SetFoldingModel, which already refuses to wipe widths.
  layout_cache_.DropWrappedRowLayouts();
}

TextViewport& TextViewport::operator=(const TextViewport& other) {
  if (this == &other) {
    return *this;
  }
  TextViewport copy(other);
  *this = std::move(copy);
  return *this;
}

TextViewport::TextViewport(TextViewport&& other) noexcept
    : fold_edit_span_(other.fold_edit_span_),
      document_(other.document_),
      cursor_line_(other.cursor_line_),
      cursor_column_(other.cursor_column_),
      preferred_column_(other.preferred_column_),
      caret_navigation_content_revision_(other.caret_navigation_content_revision_),
      scroll_line_(other.scroll_line_),
      horizontal_scroll_(other.horizontal_scroll_),
      visible_lines_(other.visible_lines_),
      visible_columns_(other.visible_columns_),
      tab_size_(other.tab_size_),
      indent_width_(other.indent_width_),
      soft_tabs_(other.soft_tabs_),
      soft_wrap_(other.soft_wrap_),
      save_trim_trailing_whitespace_(other.save_trim_trailing_whitespace_),
      save_ensure_final_newline_(other.save_ensure_final_newline_),
      save_line_ending_override_(other.save_line_ending_override_),
      lc_view_(std::move(other.lc_view_)),
      language_id_(std::move(other.language_id_)),
      language_id_document_(other.language_id_document_),
      language_id_path_(std::move(other.language_id_path_)),
      language_id_content_revision_(other.language_id_content_revision_),
      language_id_registry_revision_(other.language_id_registry_revision_),
      language_id_valid_(other.language_id_valid_),
      secondary_carets_(std::move(other.secondary_carets_)),
      column_selection_(other.column_selection_),
      secondary_caret_positions_cache_(std::move(other.secondary_caret_positions_cache_)),
      // Moved, not dropped: stealing the buffers is free and keeps the moved-to
      // viewport's first box-selection rebuild allocation-free.
      box_ranges_scratch_(std::move(other.box_ranges_scratch_)),
      secondary_caret_candidates_scratch_(std::move(other.secondary_caret_candidates_scratch_)),
      layout_cache_(std::move(other.layout_cache_)),
      highlight_cache_(std::move(other.highlight_cache_)),
      highlight_cache_order_(std::move(other.highlight_cache_order_)),
      initial_highlight_state_(std::move(other.initial_highlight_state_)),
      line_highlight_states_(std::move(other.line_highlight_states_)),
      line_highlight_states_valid_through_(other.line_highlight_states_valid_through_),
      highlight_checkpoints_(std::move(other.highlight_checkpoints_)),
      highlight_checkpoints_valid_through_(other.highlight_checkpoints_valid_through_),
      pending_checkpoint_backfill_target_line_(other.pending_checkpoint_backfill_target_line_),
      last_highlight_state_exact_(other.last_highlight_state_exact_),
      highlight_state_content_revision_(other.highlight_state_content_revision_),
      highlight_state_syntax_revision_(other.highlight_state_syntax_revision_),
      highlight_queries_(other.highlight_queries_),
      highlight_hits_(other.highlight_hits_),
      highlight_state_advances_(other.highlight_state_advances_),
      highlight_checkpoint_advances_(other.highlight_checkpoint_advances_),
      selection_anchor_(std::move(other.selection_anchor_)),
      last_applied_edit_(std::move(other.last_applied_edit_)),
      last_applied_edit_line_span_(std::move(other.last_applied_edit_line_span_)),
      folding_model_(nullptr),
      undo_history_(std::move(other.undo_history_)) {
  other.folding_model_ = nullptr;
  other.layout_cache_ = TextLayoutCache{};
  other.undo_history_ = TextViewportUndoHistory{};
  // See the copy constructor: the moved-in width table describes the same
  // document at the same tab size, so only the fold-dependent half goes.
  layout_cache_.DropWrappedRowLayouts();
}

TextViewport& TextViewport::operator=(TextViewport&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  fold_edit_span_ = other.fold_edit_span_;
  document_ = other.document_;
  cursor_line_ = other.cursor_line_;
  cursor_column_ = other.cursor_column_;
  preferred_column_ = other.preferred_column_;
  caret_navigation_content_revision_ = other.caret_navigation_content_revision_;
  scroll_line_ = other.scroll_line_;
  horizontal_scroll_ = other.horizontal_scroll_;
  visible_lines_ = other.visible_lines_;
  visible_columns_ = other.visible_columns_;
  tab_size_ = other.tab_size_;
  indent_width_ = other.indent_width_;
  soft_tabs_ = other.soft_tabs_;
  soft_wrap_ = other.soft_wrap_;
  save_trim_trailing_whitespace_ = other.save_trim_trailing_whitespace_;
  save_ensure_final_newline_ = other.save_ensure_final_newline_;
  save_line_ending_override_ = other.save_line_ending_override_;
  lc_view_ = std::move(other.lc_view_);
  language_id_ = std::move(other.language_id_);
  language_id_document_ = other.language_id_document_;
  language_id_path_ = std::move(other.language_id_path_);
  language_id_content_revision_ = other.language_id_content_revision_;
  language_id_registry_revision_ = other.language_id_registry_revision_;
  language_id_valid_ = other.language_id_valid_;
  secondary_carets_ = std::move(other.secondary_carets_);
  column_selection_ = other.column_selection_;
  secondary_caret_positions_cache_ = std::move(other.secondary_caret_positions_cache_);
  box_ranges_scratch_ = std::move(other.box_ranges_scratch_);
  secondary_caret_candidates_scratch_ = std::move(other.secondary_caret_candidates_scratch_);
  layout_cache_ = std::move(other.layout_cache_);
  highlight_cache_ = std::move(other.highlight_cache_);
  highlight_cache_order_ = std::move(other.highlight_cache_order_);
  initial_highlight_state_ = std::move(other.initial_highlight_state_);
  line_highlight_states_ = std::move(other.line_highlight_states_);
  line_highlight_states_valid_through_ = other.line_highlight_states_valid_through_;
  highlight_checkpoints_ = std::move(other.highlight_checkpoints_);
  highlight_checkpoints_valid_through_ = other.highlight_checkpoints_valid_through_;
  pending_checkpoint_backfill_target_line_ = other.pending_checkpoint_backfill_target_line_;
  last_highlight_state_exact_ = other.last_highlight_state_exact_;
  highlight_state_content_revision_ = other.highlight_state_content_revision_;
  highlight_state_syntax_revision_ = other.highlight_state_syntax_revision_;
  highlight_queries_ = other.highlight_queries_;
  highlight_hits_ = other.highlight_hits_;
  highlight_state_advances_ = other.highlight_state_advances_;
  highlight_checkpoint_advances_ = other.highlight_checkpoint_advances_;
  selection_anchor_ = std::move(other.selection_anchor_);
  last_applied_edit_ = std::move(other.last_applied_edit_);
  last_applied_edit_line_span_ = std::move(other.last_applied_edit_line_span_);
  folding_model_ = nullptr;
  undo_history_ = std::move(other.undo_history_);
  other.folding_model_ = nullptr;
  other.layout_cache_ = TextLayoutCache{};
  other.undo_history_ = TextViewportUndoHistory{};
  // See the copy constructor: the moved-in width table describes the same
  // document at the same tab size, so only the fold-dependent half goes.
  layout_cache_.DropWrappedRowLayouts();
  return *this;
}

const std::string& TextViewport::language_id() const {
  util::AddPerformanceCounter(util::PerfCounterId::EditorFiletypeMemoQueries);
  const void* document = document_.get();
  const std::uint64_t revision = content_revision();
  const std::size_t registry_revision = runtime_syntax::RegistryRevision();
  if (language_id_valid_ && language_id_document_ == document &&
      language_id_content_revision_ == revision &&
      language_id_registry_revision_ == registry_revision &&
      (document == nullptr || language_id_path_ == document_->path)) {
    util::AddPerformanceCounter(util::PerfCounterId::EditorFiletypeMemoHits);
    return language_id_;
  }
  if (document == nullptr) {
    language_id_.clear();
  } else {
    // Bounded head scan only -- LineSpan reads through the live buffer, so this
    // never materializes the document.
    language_id_ = runtime_syntax::DetectFiletype(document_->path, LineSpan(document_->lines));
    language_id_path_ = document_->path;
  }
  language_id_document_ = document;
  language_id_content_revision_ = revision;
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
  // Through VisualColumnAt, not the raw walk: this runs once per rendered row per
  // frame, so on a line with no newlines in it the raw form re-walked to the caret
  // -- a megabyte -- on every frame, which is what the render path's remaining
  // self time turned out to be once the layout and folding passes were fixed.
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

std::size_t TextViewport::WrappedLineRowOffset(std::size_t line_index) const {
  return layout_cache_.WrappedLineRowOffset(line_index);
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
  EnsureWrappedRowLayouts();
  if (visual_row_index >= WrappedRowCount()) {
    return LayoutLine{};
  }

  const WrappedRowLayout row = WrappedRowAt(visual_row_index);
  const std::size_t row_columns =
      row.visual_end > row.visual_start ? row.visual_end - row.visual_start : 0;
  LayoutLine layout = TextLayout::BuildVisibleLine(
      document_->lines.LineView(row.line_index), row.visual_start,
      std::min(visible_columns_, row_columns), tab_size_);
  if (row.line_index == cursor_line_ && visual_row_index == cursor_visual_row) {
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
      .indent = row.indent,
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
  // Continuation rows render their content shifted right by `indent` cells, so
  // subtract that gutter before mapping the click to a content column.
  const std::size_t hit_col = static_cast<std::size_t>(std::max(0, visual_col));
  const std::size_t local = hit_col > layout.indent ? hit_col - layout.indent : 0;
  const std::size_t clamped_local = std::min<std::size_t>(local, width);
  const std::size_t target_visual = layout.visual_start + clamped_local;
  return LogicalPosition{
      .line = layout.line_index,
      .column = TextColumnAtVisualColumn(layout.line_index, target_visual),
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
  // Quick reject: matching sizes + identical elements means the cache is current and we can
  // hand out the existing view without touching the heap.
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
  // secondary_carets_ is sorted by position at every mutation site that leaves
  // this class (SetSecondaryCarets, AddSecondaryCaretWithRange, the column-caret
  // build, and the post-edit remap all sort; the ViewState restore copies an
  // already-sorted vector), so the duplicate check and the insertion point are
  // one binary probe. The previous form paid a linear find_if AND a full re-sort
  // per insert, so a run of Ctrl+clicks on top of an existing column selection
  // (capped at kMaxColumnCarets = 10,000) was O(k * n log n) for no reason — the
  // same redundant-re-sort problem SetSecondaryCarets was already fixed for.
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
}

void TextViewport::AddSecondaryCaretWithRange(SelectionRange range) {
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
  std::sort(secondary_carets_.begin(), secondary_carets_.end(),
            detail::SecondaryCaretPositionLess);
  DedupeSecondaryCaretsAgainstPrimary();
}

void TextViewport::SetSecondaryCarets(std::vector<TextPosition> carets) {
  // Single-pass rebuild: clamp every position, sort once, then emit while
  // dropping duplicates and the primary caret. The previous implementation
  // called AddSecondaryCaret in a loop, and AddSecondaryCaret itself does a
  // linear find_if plus a full std::sort on every insert -- so rebuilding k
  // carets was O(k^2 log k), redundantly re-sorting an already-clamped set.
  // This is O(k log k) and produces the identical final set/order (clamping is
  // monotonic, so sorting after clamping matches AddSecondaryCaret's
  // sort-by-clamped-position, and the primary skip mirrors its cursor check).
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
  // Ranged rebuild mirroring SetSecondaryCarets, but each caret keeps its
  // selection anchor. Ctrl+D ("add cursor at next/all match") needs the
  // secondaries to retain their match selection so a following keystroke
  // REPLACES each occurrence (and copy aggregates them); routing bare positions
  // through SetSecondaryCarets drops the anchors and corrupts the edit. Build
  // all candidates, sort once, then drop duplicates and the primary -- O(k log k)
  // instead of the O(k^2 log k) of calling AddSecondaryCaretWithRange in a loop.
  secondary_carets_.clear();
  if (ranges.empty() || document_->lines.empty()) {
    return;
  }
  const TextPosition primary{cursor_line_, cursor_column_};
  std::vector<SecondaryCaret>& candidates = secondary_caret_candidates_scratch_;
  candidates.clear();
  // Geometric, not exact: a held column-select gesture calls this with a span one
  // line longer each keystroke, and `reserve` allocates exactly what is asked for.
  util::ReserveGrowing(candidates, ranges.size());
  for (const SelectionRange& range : ranges) {
    const SelectionRange norm = NormalizeRange(range);
    if (!detail::ValidateRangeColumns(document_->lines, norm)) {
      continue;
    }
    // Bounds-check via the normalized range, but keep the caret on the side the
    // caller specified: `range.start` is the anchor and `range.end` is the caret.
    // A leftward box drag (or a line-move restore of a caret-before-anchor
    // secondary selection) passes an inverted range whose caret must stay at
    // `range.end`; normalizing to max would strand every secondary caret on the
    // opposite edge from the primary, breaking column alignment and shift-extension.
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
  // Both production callers already produce candidates in document order — the box
  // rebuild walks lines low→high, and the Ctrl+D match scan walks the buffer
  // forwards — so the common case is an O(k) verification instead of an O(k log k)
  // sort. std::sort on sorted input is not free: it still recurses and partitions.
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
    secondary_carets_.push_back(std::move(candidate));
  }
}

void TextViewport::ClearSecondaryCarets() {
  secondary_carets_.clear();
}

void TextViewport::PlaceColumnCaretsBetweenLines(std::size_t anchor_line,
                                                 std::size_t target_line,
                                                 std::size_t column) {
  // Zero-width column carets are the degenerate box selection where both corners
  // share `column`; delegate so the span cap and caret-set construction live once.
  SetBoxSelection(TextPosition{anchor_line, column}, TextPosition{target_line, column});
}

std::size_t TextViewport::MaxLineLengthInSpan(std::size_t lo, std::size_t hi) const {
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
    longest = std::max(longest, document_->lines.LineLength(line));
  }
  return longest;
}

void TextViewport::SetBoxSelection(TextPosition anchor, TextPosition caret) {
  util::PerformanceTrace::Scope trace_scope("TextViewport::SetBoxSelection");
  if (document_->lines.empty()) {
    return;
  }
  util::AddPerformanceCounter(util::PerfCounterId::EditorBoxSelectionBuilds);
  const std::size_t last_line = document_->lines.size() - 1;
  anchor.line = std::min(anchor.line, last_line);
  caret.line = std::min(caret.line, last_line);

  std::size_t lo = std::min(anchor.line, caret.line);
  std::size_t hi = std::max(anchor.line, caret.line);

  // Cap the caret span. A column/box-select gesture across a multi-million-line
  // file would otherwise allocate one caret per line (and every later multi-caret
  // edit becomes O(N) per keystroke) — a single-gesture OOM/hang. Keep the window
  // nearest the drag target, where the user is actually working. A real column
  // edit spans a modest number of lines.
  constexpr std::size_t kMaxColumnCarets = 10000;
  if (hi - lo + 1 > kMaxColumnCarets) {
    if (caret.line >= hi) {
      lo = hi - (kMaxColumnCarets - 1);
    } else {
      hi = lo + (kMaxColumnCarets - 1);
    }
  }

  // Virtual box columns. Each line clamps them to its own length so a short line
  // collapses to a zero-width caret at end-of-line instead of dropping out.
  const std::size_t anchor_column = anchor.column;
  const std::size_t caret_column = caret.column;

  ClearSecondaryCarets();

  // Build the ranged secondary set once (single clamp+sort+dedupe pass in
  // SetSecondaryCaretsWithRanges) for every line except the primary/caret line.
  // Columns are pre-clamped to each line's length so ValidateRangeColumns accepts
  // them; an anchor==caret range on a line yields a zero-width caret.
  util::AddPerformanceCounter(util::PerfCounterId::EditorBoxSelectionCaretsPlaced, hi - lo + 1);
  // Reused across keystrokes: a held column-select gesture rebuilds the whole set
  // every step, so a fresh vector here is one growing allocation per keystroke.
  std::vector<SelectionRange>& ranges = box_ranges_scratch_;
  ranges.clear();
  util::ReserveGrowing(ranges, hi - lo);
  for (std::size_t line = lo; line <= hi; ++line) {
    if (line == caret.line) {
      continue;
    }
    const std::size_t line_len = document_->lines.LineLength(line);
    const std::size_t a = std::min(anchor_column, line_len);
    const std::size_t c = std::min(caret_column, line_len);
    ranges.push_back(SelectionRange{TextPosition{line, a}, TextPosition{line, c}});
  }

  // Primary caret+selection on the caret line: land on the anchor column (clearing
  // any prior selection), then extend to the caret column so the primary row spans
  // the box. Equal columns leave an empty selection -> a plain column caret.
  const std::size_t caret_line_len = document_->lines.LineLength(caret.line);
  MoveCursorTo(caret.line, std::min(anchor_column, caret_line_len), false);
  MoveCursorTo(caret.line, std::min(caret_column, caret_line_len), true);
  SetSecondaryCaretsWithRanges(ranges);
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

std::string TextViewport::CurrentLineTextForClipboard() const {
  if (document_->lines.empty()) {
    return {};
  }

  std::string text;
  text.reserve(document_->lines.LineLength(cursor_line_) + 1);
  text += document_->lines.LineView(cursor_line_);
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

    const std::size_t before_document_line_count = document_->lines.size();
    const std::size_t before_lines_start = lines_to_delete.front();
    const std::size_t before_lines_end = lines_to_delete.back() + 1;
    std::vector<std::string> before_lines =
        document_->lines.SliceLines(before_lines_start, before_lines_end);
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
    // Multi-line delete is a content edit; bump the content tier.
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
        document_->lines.SliceLines(after_lines_start, after_lines_end), CaptureViewState());
    aggregate_entry.start_line += before_lines_start;
    // A disjoint multi-line delete cannot be described by one contiguous
    // AppliedEdit; leaving the previous single-caret edit's value in place would
    // feed a stale range/replacement to the incremental LSP sync and the
    // breakpoint shifter. Clear it so both fall back to a full resync, matching
    // the sibling aggregate paths (TextViewportMultiCaret / LanguageBehavior).
    ClearLastAppliedEdit();
    PushHistoryEntry(std::move(aggregate_entry));
    return true;
  }

  if (document_->lines.size() == 1) {
    return ApplyRangeEdit(SelectionRange{
                              .start = TextPosition{0, 0},
                              .end = TextPosition{0, document_->lines.LineLength(0)},
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
                                document_->lines.LineLength(previous_line),
                            },
                            .end = TextPosition{cursor_line_, document_->lines.LineLength(cursor_line_)},
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
  cursor_column_ = document_->lines.LineLength(cursor_line_);
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  EnsureCursorVisible();
}

void TextViewport::SelectWordAtCursor() {
  if (document_->lines.empty()) {
    return;
  }
  const std::string_view line = document_->lines.LineView(cursor_line_);
  const std::size_t col = std::min(cursor_column_, line.size());
  std::size_t start = col;
  std::size_t end = col;
  if (col < line.size() && IsIdentifierByte(line[col])) {
    while (start > 0 && IsIdentifierByte(line[start - 1])) {
      --start;
    }
    while (end < line.size() && IsIdentifierByte(line[end])) {
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
  std::size_t anchor_col = col;
  if (col < line.size() && IsIdentifierByte(line[col])) {
    // Primary caret indexes a word character.
  } else if (col > 0 && IsIdentifierByte(line[col - 1])) {
    anchor_col = col - 1;
  } else {
    return std::nullopt;
  }

  std::size_t start = anchor_col;
  std::size_t end = anchor_col;
  while (start > 0 && IsIdentifierByte(line[start - 1])) {
    --start;
  }
  while (end < line.size() && IsIdentifierByte(line[end])) {
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
  cursor_column_ = document_->lines.LineLength(cursor_line_);
  preferred_column_ = PreferredColumnForCaret(TextPosition{cursor_line_, cursor_column_});
  EnsureCursorVisible();
}

void TextViewport::SetDocumentPath(const std::filesystem::path& path) {
  document_->path = path;
  document_->path_key = NormalizedPathKey(path);
}

void TextViewport::ResetState(std::vector<std::string> lines,
                              const std::filesystem::path& path,
                              LineEnding line_ending,
                              bool mixed_line_endings,
                              TextEncoding encoding,
                              bool placeholder,
                              bool dirty) {
  EnsureDocument();
  document_->lines.Reset(lines.empty() ? std::vector<std::string>{""} : std::move(lines));
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
  cursor_line_ = 0;
  cursor_column_ = 0;
  preferred_column_ = 0;
  scroll_line_ = 0;
  horizontal_scroll_ = 0;
  selection_anchor_.reset();
  secondary_carets_.clear();
  undo_history_.Clear();
  document_->placeholder = placeholder;
  document_->dirty = dirty;
  // Capture the file's on-disk identity as our conflict-detection baseline. For
  // untitled buffers (empty path) this records "absent", so first saves are
  // never treated as conflicts.
  document_->disk_signature = util::StatFileSignature(path);
  InvalidateVisualColumnCache();
  // ResetState replaces every line; classify as a content edit so the
  // content tier reflects the change and dependent caches are dropped.
  // ResetState replaces every line, so the invalidation above already marked the
  // whole fold span dirty -- which is what the model's per-line caches need. The
  // old code reset the fold anchor to its idle sentinel here; with a real cache
  // downstream that would claim a freshly loaded document matches the previous
  // document's cached lines.
  InvalidateDerivedCaches(InvalidationReason::ContentEdit, 0);
  EnsureCursorVisible();
}

void TextViewport::InvalidateDerivedCaches(InvalidationReason reason) {
  InvalidateDerivedCaches(reason, 0);
}

void TextViewport::InvalidateDerivedCaches(InvalidationReason reason, std::size_t start_line,
                                           std::optional<ContentSplice> splice) {
  EnsureDocument();
  // Tier fan-out: each reason bumps exactly the tiers it implies. Every
  // reason bumps presentation_revision because any cause of invalidation
  // changes at least the rendered pixels. Counters are incremented per tier
  // bump so a smoke run can assert which tiers moved.
  ++document_->presentation_revision;
  util::AddPerformanceCounter(util::PerfCounterId::EditorPresentationRevisionBumps);
  if (reason == InvalidationReason::ContentEdit) {
    ++document_->content_revision;
    util::AddPerformanceCounter(util::PerfCounterId::EditorContentRevisionBumps);
  } else if (reason == InvalidationReason::SyntaxConfig) {
    ++document_->syntax_revision;
    util::AddPerformanceCounter(util::PerfCounterId::EditorSyntaxRevisionBumps);
  } else if (reason == InvalidationReason::LayoutShape) {
    ++document_->layout_shape_revision;
    util::AddPerformanceCounter(util::PerfCounterId::EditorLayoutShapeRevisionBumps);
  }

  // Caches below are content-derived (visible-line layout, highlight tokens,
  // fold edit anchor). They only need rebuilding on a content edit; pure
  // syntax/layout-shape/presentation invalidations do not touch them.
  if (reason != InvalidationReason::ContentEdit) {
    if (reason == InvalidationReason::SyntaxConfig) {
      // Highlight cache depends on syntax_revision. Drop it fully — the
      // start_line argument is meaningless for a theme/contract change.
      highlight_cache_.clear();
      highlight_cache_order_.clear();
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
  util::AddPerformanceCounter(util::PerfCounterId::EditorInvalidateDerivedCachesCalls);
  util::AddPerformanceCounter(util::PerfCounterId::EditorInvalidateDerivedCachesLines,
                              document_->lines.size() - safe_start);

  // Accumulate what the folding model has to resync. A reported splice gives it
  // an exact window, so its per-line indent/bracket caches keep everything
  // outside that window; without one, nothing from `safe_start` on is reusable.
  if (splice.has_value()) {
    fold_edit_span_.NoteSplice(safe_start, splice->removed, splice->inserted);
  } else {
    fold_edit_span_.NoteSuffixReplaced(safe_start);
  }

  if (safe_start == 0) {
    // Drop the visible-line layouts from line 0 -- i.e. all of them -- but NOT the
    // per-line visual-column table, which is what ClearVisibleLineAndMaxColumns
    // would also have taken.
    //
    // Line 0 is special for the highlight state below, because the syntax state
    // chains forward from it. It is not special for widths: a line's visual column
    // count depends on that line's bytes and nothing else, and the incremental
    // UpdateVisualColumnCacheAfterEdit that every edit path runs right after this
    // handles line 0 exactly like any other line. Wiping the table here meant the
    // next MaxVisualColumns() rebuilt the width of every line in the buffer, so
    // an edit anchored at line 0 cost an O(document) rebuild that the same edit
    // one line lower did not: on the 50k-line fixture, an identical
    // enter/backspace burst measured 121 full rebuilds (269.7 ms) at line 0
    // against 1 (3.1 ms) at line 25000. Typing at the top of a large file was the
    // slowest place to type.
    layout_cache_.InvalidateVisibleLineCacheFrom(0);
    highlight_cache_.clear();
    highlight_cache_order_.clear();
    initial_highlight_state_.reset();
    // Drop the validity cursors, NOT the storage. Every read of these two is
    // gated on its cursor (`line < line_highlight_states_valid_through_`,
    // `index < highlight_checkpoints_valid_through_`), so stale entries below a
    // zeroed cursor are unreachable -- while clearing the vectors made
    // EnsureHighlightCaches resize them straight back, value-initialising ~50k
    // SyntaxStates (2 MB) on every keystroke. This is the same lazy invalidation
    // the non-zero-start path below already documents; the two had simply drifted.
    line_highlight_states_valid_through_ = 0;
    highlight_checkpoints_valid_through_ = 0;
    highlight_state_content_revision_ = document_->content_revision;
    highlight_state_syntax_revision_ = document_->syntax_revision;
    return;
  }

  layout_cache_.InvalidateVisibleLineCacheFrom(safe_start);

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
    line_highlight_states_.resize(document_->lines.size());
  }
  // Lazy invalidation: drop the validity cursor instead of looping
  // SyntaxState{} into ~50 000 entries on every keystroke.
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
    highlight_checkpoints_.front() = *initial_highlight_state_;
    if (highlight_checkpoints_valid_through_ < 1) {
      highlight_checkpoints_valid_through_ = 1;
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
  // The only remaining caller (ReplaceAll) just mutated buffer content, so
  // route this through the content tier. We still wipe the visual-column
  // cache because the column metrics depend on the new line widths.
  InvalidateVisualColumnCache();
  InvalidateDerivedCaches(InvalidationReason::ContentEdit, 0);
}

void TextViewport::InvalidateSyntaxHighlighting() {
  // External callers invoke this when language identification or theme
  // configuration changes — a syntax-tier mutation, not a content edit.
  // The visual-column cache and wrapped-row layouts are unaffected by a
  // syntax change, so leave them in place.
  InvalidateDerivedCaches(InvalidationReason::SyntaxConfig, 0);
}

void TextViewport::BeginSelectionIfNeeded(bool extend_selection) {
  // Anchoring/collapsing must be symmetric across EVERY caret, primary and
  // secondary. Each move method advances the secondary positions in its own loop;
  // this only fixes up the anchors, before those positions move.
  if (extend_selection) {
    // Extending (Shift) move: start a selection at any caret that lacks one so a
    // multi-caret Shift+Arrow selects at every cursor (VSCode), not just the
    // primary. Carets that already own a selection (Ctrl+D / box set) keep it.
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
  // Plain (non-extending) move collapses EVERY caret's selection. Without this a
  // Ctrl+D / box-select set kept ghost selections on the secondary carets after an
  // arrow key: they still rendered, and the next keystroke REPLACED those stale
  // selections (or the edit was refused as overlapping) instead of inserting at the
  // collapsed caret -- diverging from the primary and from VSCode.
  selection_anchor_.reset();
  for (SecondaryCaret& caret : secondary_carets_) {
    caret.selection_anchor.reset();
  }
}

void TextViewport::UpdateVisualColumnCacheAfterEdit(std::size_t start_line,
                                                    std::size_t removed_count,
                                                    std::size_t inserted_count,
                                                    InlineLineSplice splice) {
  layout_cache_.UpdateVisualColumnCacheAfterEdit(start_line, removed_count, inserted_count,
                                                  document_->lines, tab_size_,
                                                  document_->content_revision, splice);
}

void TextViewport::UpdateWrappedRowsAfterEdit(std::size_t start_line,
                                              std::size_t removed_count,
                                              std::size_t inserted_count) {
  // Fast path for the pure soft-wrap case: splice only the edited rows. Any
  // unsupported shape returns false and the content_revision guard rebuilds.
  layout_cache_.UpdateWrappedRowsAfterEdit(start_line, removed_count, inserted_count,
                                           document_->lines, tab_size_, visible_columns_,
                                           soft_wrap_, folding_model_,
                                           document_->layout_shape_revision,
                                           document_->content_revision);
}

std::size_t TextViewport::MaxVisualColumns() const {
  return layout_cache_.MaxVisualColumns(document_->lines, tab_size_, document_->content_revision);
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

}  // namespace microide::editor
