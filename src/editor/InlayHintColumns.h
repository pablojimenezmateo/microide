#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "editor/PluginDecorationStore.h"
#include "editor/TextLayout.h"
#include "render/TextRenderer.h"

namespace microide::editor {

// One mid-line inlay hint reduced to grid geometry within a single visual row:
// the (row-local) visual column it renders immediately before, and how many whole
// grid cells its measured text occupies. "Anchored before column a" means the
// hint's phantom cells sit to the left of the real glyph at column a, pushing it
// (and everything after) rightward.
struct InlayCellSpan {
  std::size_t anchor_visual_column = 0;  // row-local visual column
  std::size_t cell_width = 0;            // whole grid cells the hint text spans
  std::size_t source_index = 0;          // index into the line's inline_texts slice
};

// Per-row phantom-cell displacement produced by mid-line inlay hints. The real
// glyph at row-local visual column v is drawn at DISPLAY column
// v + CellsInsertedBefore(v); every column->x consumer on the row (text runs,
// caret, fills, underlines, whitespace, end-of-line anchor) adds
// CellsInsertedBefore(v) * char_width to stay aligned, and the mouse hit-test
// inverts it via VisualColumnForDisplayColumn.
//
// All columns are row-local (measured from the row's first visible visual
// column). Spans MUST be sorted by anchor_visual_column ascending; source-column
// order already guarantees this. Empty => identity at O(1) so a row without hints
// (the overwhelmingly common case) pays a single emptiness branch.
class InlayRowDisplacement {
 public:
  InlayRowDisplacement() = default;
  explicit InlayRowDisplacement(std::span<const InlayCellSpan> spans) : spans_(spans) {}

  bool empty() const { return spans_.empty(); }
  std::span<const InlayCellSpan> spans() const { return spans_; }

  // Phantom cells inserted before the real glyph at visual column v. A hint
  // anchored AT v precedes that glyph, so its width counts here.
  std::size_t CellsInsertedBefore(std::size_t visual_column) const;

  // Total phantom cells inserted across the whole row.
  std::size_t TotalInsertedCells() const;

  // The smallest hint anchor >= visual_column, or SIZE_MAX when none. Lets a
  // run-coalescing pass break a run exactly where the display displacement steps.
  std::size_t NextAnchorAtOrAfter(std::size_t visual_column) const;

  // Hit-test inverse: map a display column (a grid cell counting phantom cells,
  // row-local) back to the real visual column. A click landing inside a hint's
  // phantom region snaps to that hint's anchor column.
  std::size_t VisualColumnForDisplayColumn(std::size_t display_column) const;

 private:
  std::span<const InlayCellSpan> spans_;
};

// Whole grid cells the inlay-hint `text` occupies at the given cell width (>= 1).
std::size_t InlayHintCellWidth(const render::TextRenderer& text_renderer, std::string_view text,
                               float char_width);

// Resolve a logical line's mid-line inlay-hint decorations (those with
// anchor_column != kInlineTextEndOfLine) into row-local InlayCellSpans for the
// visible window [row_visual_start, row_visual_end). EOL-anchored inline texts are
// ignored here (they route through BuildEolDecorationSegments). `layout` (grid
// path) or `visual_map` maps a hint's source byte column to a visual column; pass
// both null to treat source columns as visual columns. `out_spans` is cleared and
// filled (caller-reused; growth is the only allocation). `*out_total_line_cells`,
// when non-null, receives the phantom-cell total across ALL mid-line hints on the
// line — used to push the end-of-line decoration anchor past the shifted glyphs.
void BuildInlayRowSpans(std::span<const InlineTextDecoration> inline_texts,
                        const LayoutLine* layout,
                        const TextLayout::LineVisualColumnMap* visual_map,
                        std::size_t row_visual_start, std::size_t row_visual_end,
                        const render::TextRenderer& text_renderer, float char_width,
                        std::vector<InlayCellSpan>& out_spans,
                        std::size_t* out_total_line_cells);

// Mouse hit-test inverse: map an absolute DISPLAY visual column (what
// `row_visual_start + round((click_x - text_x) / char_width)` yields, counting the
// on-screen phantom inlay cells) back to the real absolute visual column a click
// targets. Returns `display_visual_column` unchanged when the row has no mid-line
// hints. A click inside a hint snaps to the annotated glyph's column. Allocates a
// small scratch internally; intended for the (cold) click path, not per-frame.
std::size_t RealVisualColumnForDisplayColumn(
    std::span<const InlineTextDecoration> inline_texts, const LayoutLine* layout,
    const TextLayout::LineVisualColumnMap* visual_map, std::size_t row_visual_start,
    std::size_t row_visual_end, const render::TextRenderer& text_renderer, float char_width,
    std::size_t display_visual_column);

}  // namespace microide::editor
