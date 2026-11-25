#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "editor/PluginDecorationStore.h"
#include "editor/TextLayout.h"
#include "render/TextRenderer.h"

namespace microide::editor {

struct InlayCellSpan {
  std::size_t anchor_visual_column = 0;
  std::size_t cell_width = 0;
  std::size_t source_index = 0;
};

class InlayRowDisplacement {
 public:
  InlayRowDisplacement() = default;
  explicit InlayRowDisplacement(std::span<const InlayCellSpan> spans) : spans_(spans) {}

  bool empty() const { return spans_.empty(); }
  std::span<const InlayCellSpan> spans() const { return spans_; }

  std::size_t CellsInsertedBefore(std::size_t visual_column) const;

  std::size_t TotalInsertedCells() const;

  std::size_t NextAnchorAtOrAfter(std::size_t visual_column) const;

  std::size_t VisualColumnForDisplayColumn(std::size_t display_column) const;

 private:
  std::span<const InlayCellSpan> spans_;
};

std::size_t InlayHintCellWidth(const render::TextRenderer& text_renderer, std::string_view text,
                               float char_width);

std::size_t InlayLineTotalCells(std::span<const InlineTextDecoration> inline_texts,
                                const render::TextRenderer& text_renderer, float char_width);

void BuildInlayRowSpans(std::span<const InlineTextDecoration> inline_texts,
                        const LayoutLine* layout,
                        const TextLayout::LineVisualColumnMap* visual_map,
                        std::size_t row_visual_start, std::size_t row_visual_end,
                        const render::TextRenderer& text_renderer, float char_width,
                        std::vector<InlayCellSpan>& out_spans,
                        std::size_t* out_total_line_cells);

std::size_t RealVisualColumnForDisplayColumn(
    std::span<const InlineTextDecoration> inline_texts, const LayoutLine* layout,
    const TextLayout::LineVisualColumnMap* visual_map, std::size_t row_visual_start,
    std::size_t row_visual_end, const render::TextRenderer& text_renderer, float char_width,
    std::size_t display_visual_column);

}
