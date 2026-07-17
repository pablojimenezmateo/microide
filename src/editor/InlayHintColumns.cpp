#include "editor/InlayHintColumns.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "util/SaturatingMath.h"

namespace microide::editor {

namespace {
// TD-2026-07-17-070: inlay hints are external plugin/LSP data. A per-hint cell
// width and per-line total that grow without a cap can wrap std::size_t or make
// the phantom-cell displacement far larger than any viewport can use, corrupting
// hit-testing / display-column mapping. No single legitimate hint spans more than
// a few dozen cells; 64K is a generous ceiling that never clips a real hint while
// bounding a hostile one, and saturating totals guarantee the displacement math
// stays monotonic even under thousands of maxed-out hints.
constexpr std::size_t kMaxInlayHintCells = 1u << 16;  // 65536 cells per hint
}  // namespace

std::size_t InlayHintCellWidth(const render::TextRenderer& text_renderer, std::string_view text,
                               float char_width) {
  const float width = text_renderer.MeasureWidth(text);
  const float cell = std::max(1.0f, char_width);
  const std::size_t cells = static_cast<std::size_t>(std::ceil(width / cell - 1e-4f));
  return std::clamp<std::size_t>(cells, 1, kMaxInlayHintCells);
}

std::size_t InlayLineTotalCells(std::span<const InlineTextDecoration> inline_texts,
                                const render::TextRenderer& text_renderer, float char_width) {
  std::size_t total = 0;
  for (const InlineTextDecoration& inl : inline_texts) {
    if (inl.anchor_column == kInlineTextEndOfLine) {
      continue;
    }
    total = util::SaturatingAdd(total, InlayHintCellWidth(text_renderer, inl.text, char_width));
  }
  return total;
}

namespace {

std::size_t ResolveInlayVisualColumn(const LayoutLine* layout,
                                     const TextLayout::LineVisualColumnMap* visual_map,
                                     std::size_t row_visual_start, std::size_t row_visual_end,
                                     std::size_t source_column) {
  if (layout != nullptr) {
    return TextLayout::VisualColumnFromLayoutClipped(*layout, row_visual_start, row_visual_end,
                                                     source_column);
  }
  if (visual_map != nullptr) {
    return visual_map->VisualColumnFor(source_column);
  }
  return source_column;
}

}  // namespace

void BuildInlayRowSpans(std::span<const InlineTextDecoration> inline_texts,
                        const LayoutLine* layout,
                        const TextLayout::LineVisualColumnMap* visual_map,
                        std::size_t row_visual_start, std::size_t row_visual_end,
                        const render::TextRenderer& text_renderer, float char_width,
                        std::vector<InlayCellSpan>& out_spans,
                        std::size_t* out_total_line_cells) {
  out_spans.clear();
  std::size_t total = 0;
  for (std::size_t i = 0; i < inline_texts.size(); ++i) {
    const InlineTextDecoration& inl = inline_texts[i];
    if (inl.anchor_column == kInlineTextEndOfLine) {
      continue;  // end-of-line virtual text is handled by BuildEolDecorationSegments
    }
    const std::size_t width = InlayHintCellWidth(text_renderer, inl.text, char_width);
    total = util::SaturatingAdd(total, width);  // every mid-line hint shifts the end-of-line anchor
    const std::size_t vcol = ResolveInlayVisualColumn(layout, visual_map, row_visual_start,
                                                      row_visual_end, inl.anchor_column);
    // Drop hints whose anchor lands outside the visible window: those scrolled off
    // the left contribute no visible shift (their displacement cancels in the
    // display origin), and those off the right shift nothing on screen. The
    // clipped mapper returns < row_visual_start / > row_visual_end sentinels for
    // exactly those cases.
    if (vcol < row_visual_start || vcol > row_visual_end) {
      continue;
    }
    out_spans.push_back(InlayCellSpan{.anchor_visual_column = vcol - row_visual_start,
                                      .cell_width = width,
                                      .source_index = i});
  }
  // inline_texts arrives sorted by anchor_column (the store sorts by
  // (line, anchor_column)); source order maps monotonically to visual order, so
  // out_spans is already sorted by anchor_visual_column. stable_sort guards
  // against an unsorted slice while keeping same-column hints in publish order.
  std::stable_sort(out_spans.begin(), out_spans.end(),
                   [](const InlayCellSpan& a, const InlayCellSpan& b) {
                     return a.anchor_visual_column < b.anchor_visual_column;
                   });
  if (out_total_line_cells != nullptr) {
    *out_total_line_cells = total;
  }
}

std::size_t RealVisualColumnForDisplayColumn(
    std::span<const InlineTextDecoration> inline_texts, const LayoutLine* layout,
    const TextLayout::LineVisualColumnMap* visual_map, std::size_t row_visual_start,
    std::size_t row_visual_end, const render::TextRenderer& text_renderer, float char_width,
    std::size_t display_visual_column) {
  if (inline_texts.empty() || display_visual_column < row_visual_start) {
    return display_visual_column;
  }
  // Reused per-thread scratch: BuildInlayRowSpans clears it, and the span-based
  // InlayRowDisplacement below only references it within this call. Avoids a heap
  // allocation on every column mapping (hover/hit-testing can run this per frame).
  thread_local std::vector<InlayCellSpan> spans;
  BuildInlayRowSpans(inline_texts, layout, visual_map, row_visual_start, row_visual_end,
                     text_renderer, char_width, spans, nullptr);
  if (spans.empty()) {
    return display_visual_column;
  }
  const InlayRowDisplacement displacement(spans);
  return row_visual_start +
         displacement.VisualColumnForDisplayColumn(display_visual_column - row_visual_start);
}

std::size_t InlayRowDisplacement::CellsInsertedBefore(std::size_t visual_column) const {
  std::size_t sum = 0;
  for (const InlayCellSpan& span : spans_) {
    if (span.anchor_visual_column > visual_column) {
      break;  // spans are sorted; the rest anchor further right
    }
    sum = util::SaturatingAdd(sum, span.cell_width);
  }
  return sum;
}

std::size_t InlayRowDisplacement::TotalInsertedCells() const {
  std::size_t sum = 0;
  for (const InlayCellSpan& span : spans_) {
    sum = util::SaturatingAdd(sum, span.cell_width);
  }
  return sum;
}

std::size_t InlayRowDisplacement::NextAnchorAtOrAfter(std::size_t visual_column) const {
  for (const InlayCellSpan& span : spans_) {
    if (span.anchor_visual_column >= visual_column) {
      return span.anchor_visual_column;
    }
  }
  return std::numeric_limits<std::size_t>::max();
}

std::size_t InlayRowDisplacement::VisualColumnForDisplayColumn(std::size_t display_column) const {
  // Walk the row left-to-right. Real columns [v, anchor) map 1:1 to display
  // columns [disp, disp + (anchor - v)); each hint then inserts `cell_width`
  // phantom display cells before its anchor's real glyph. A display column inside
  // a phantom region resolves to the hint's anchor (cursor lands before the
  // annotated glyph).
  std::size_t v = 0;      // real visual column
  std::size_t disp = 0;   // display column of the real glyph at v
  for (const InlayCellSpan& span : spans_) {
    const std::size_t gap = span.anchor_visual_column - v;  // 1:1 run before the anchor
    if (display_column < disp + gap) {
      return v + (display_column - disp);
    }
    v = span.anchor_visual_column;
    disp += gap;
    if (display_column < disp + span.cell_width) {
      return v;  // inside the hint's phantom cells -> snap to the anchor
    }
    disp += span.cell_width;  // step past the phantom cells
  }
  return v + (display_column - disp);  // 1:1 tail past the last hint
}

}  // namespace microide::editor
