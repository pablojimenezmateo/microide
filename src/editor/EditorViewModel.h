#pragma once

#include <cstddef>
#include <vector>

namespace microide::editor {

struct FoldGutterMark {
  std::size_t line_index = 0;
  std::size_t visual_row_index = 0;
  bool collapsed = false;
};

struct OccurrenceRange {
  std::size_t line_index = 0;
  std::size_t start_column = 0;
  std::size_t end_column = 0;
  bool is_primary_seed = false;
};

/// One whitespace decoration for a clipped visual row fragment (tabs span
/// multiple visual cells across the wrapped-row slice intersecting `[row_visual_start,
/// row_visual_end)`).
struct WhitespaceGlyphRun {
  std::size_t visual_row_index = 0;
  std::size_t row_visual_start = 0;
  std::size_t row_visual_end = 0;
  std::size_t cell_visual_start = 0;
  std::size_t cell_visual_extent = 1;
  bool is_tab_rule = false;
};

struct EditorViewModel {
  std::vector<FoldGutterMark> fold_gutter_marks;
  std::vector<OccurrenceRange> occurrence_ranges;
  // Logical opener line indices from outer enclosing fold to inner, pinned in the sticky band
  // (top row = outer scope). Empty when sticky scroll is disabled or no enclosing folds apply.
  std::vector<std::size_t> sticky_lines;
  std::vector<WhitespaceGlyphRun> whitespace_glyph_runs;
};

}  // namespace microide::editor
