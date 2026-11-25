#include "editor/InlayHintColumns.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "util/SaturatingMath.h"

namespace microide::editor {

namespace {
constexpr std::size_t kMaxInlayHintCells = 1u << 16;
}

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
      continue;
    }
    const std::size_t width = InlayHintCellWidth(text_renderer, inl.text, char_width);
    total = util::SaturatingAdd(total, width);
    const std::size_t vcol = TextLayout::ResolveVisualColumn(layout, visual_map, row_visual_start,
                                                             row_visual_end, inl.anchor_column);
    if (vcol < row_visual_start || vcol > row_visual_end) {
      continue;
    }
    out_spans.push_back(InlayCellSpan{.anchor_visual_column = vcol - row_visual_start,
                                      .cell_width = width,
                                      .source_index = i});
  }
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
      break;
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
  std::size_t v = 0;
  std::size_t disp = 0;
  for (const InlayCellSpan& span : spans_) {
    const std::size_t gap = span.anchor_visual_column - v;
    if (display_column < disp + gap) {
      return v + (display_column - disp);
    }
    v = span.anchor_visual_column;
    disp += gap;
    if (display_column < disp + span.cell_width) {
      return v;
    }
    disp += span.cell_width;
  }
  return v + (display_column - disp);
}

}
