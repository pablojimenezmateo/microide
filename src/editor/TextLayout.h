#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace microide::editor {

struct LayoutLine {
  std::string text;
  std::vector<std::size_t> source_columns;
  std::vector<std::size_t> text_offsets;
  std::size_t visual_columns = 0;
  std::size_t caret_column = 0;
  bool caret_visible = false;
};

class TextLayout {
 public:
  static std::size_t VisualColumnForTextColumn(const std::string& line,
                                               std::size_t text_column,
                                               std::size_t tab_size);
  static std::size_t TextColumnForVisualColumn(const std::string& line,
                                               std::size_t visual_column,
                                               std::size_t tab_size);
  static std::size_t ClampTextColumn(const std::string& line, std::size_t text_column);
  static std::size_t PreviousTextColumn(const std::string& line, std::size_t text_column);
  static std::size_t NextTextColumn(const std::string& line, std::size_t text_column);
  static LayoutLine BuildVisibleLine(const std::string& line,
                                     std::size_t horizontal_scroll,
                                     std::size_t visible_columns,
                                     std::size_t tab_size);

  // O(N) prefix walk that captures every text-byte boundary's visual column for `line`. Once
  // built, `VisualColumnFor(text_column)` is O(log N). Use when more than one VisualColumn query
  // hits the same line within a frame (compare/merge row render, hover target geometry) so the
  // per-line tab/UTF-8 walk runs once instead of once per query.
  class LineVisualColumnMap {
   public:
    LineVisualColumnMap(const std::string& line, std::size_t tab_size);
    std::size_t VisualColumnFor(std::size_t text_column) const;
    std::size_t LineVisualWidth() const { return line_visual_width_; }

   private:
    // Parallel arrays: boundaries_[i] is a text byte offset (in monotonic order); visuals_[i] is
    // the visual column at that boundary. Both include 0 and line.size() as endpoints.
    std::vector<std::size_t> boundaries_;
    std::vector<std::size_t> visuals_;
    std::size_t line_visual_width_ = 0;
  };

  // Resolve a source byte column to its visual column using an already-built `LayoutLine`. This
  // avoids the O(line_length) tab-stop walk that VisualColumnForTextColumn performs.
  //
  // The layout describes only the visible cells `[row_start_visual, row_end_visual)`. For source
  // columns outside that window the returned value is intentionally beyond the window so callers
  // that clip via `std::max(start, row_start_visual)` / `std::min(end, row_end_visual)` get the
  // correct clipped value without a fallback. Specifically:
  //   - if `source_column` precedes the leftmost visible source byte: returns
  //     `row_start_visual > 0 ? row_start_visual - 1 : 0` (safe lower-bound sentinel).
  //   - if `source_column` is past the last visible cell: returns `row_end_visual + 1`.
  //   - otherwise: returns `row_start_visual + cell_index` where cell_index is the first cell
  //     in `layout.source_columns` with `source_column[cell_index] >= source_column`.
  static std::size_t VisualColumnFromLayoutClipped(const LayoutLine& layout,
                                                   std::size_t row_start_visual,
                                                   std::size_t row_end_visual,
                                                   std::size_t source_column);

 private:
  static std::size_t AdvanceVisualColumn(std::size_t visual_column,
                                         char character,
                                         std::size_t tab_size);
};

}  // namespace microide::editor
