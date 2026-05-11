#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace microide::editor {

class FoldingModel;

// One vertical indent-guide segment to be painted at `column` (visual column
// in the viewport's column coordinate space) for visible rows in the inclusive
// range [start_row, end_row]. `active` is true when the guide is the immediate
// parent of the caret's enclosing block.
struct IndentGuideRun {
  std::size_t column = 0;
  std::size_t start_row = 0;  // visible-row index (0-based, top-down)
  std::size_t end_row = 0;
  bool active = false;
};

// Compute indent guides for the lines covered by `visible_rows`. Each entry in
// `visible_rows` is the buffer line index drawn at that visible row; the
// helper assumes the renderer has already mapped soft-wrap/folding to these
// indices. `tab_size` is used to expand tabs into visual columns.
//
// `caret_line` and `caret_leading_visual_indent` flag the active guide column;
// pass `caret_line == SIZE_MAX` to disable active emphasis.
//
// `out` is cleared and refilled in place; capacity is preserved across calls
// when the helper is invoked with the same destination vector.
//
// When `folding_model` is non-null, the caret's active-indent column prefers
// the innermost enclosing fold opener's indentation (when the caret is past
// the opener line); otherwise the legacy leading-indent scan applies.
void ComputeIndentGuides(const std::vector<std::string>& lines,
                         const std::vector<std::size_t>& visible_rows,
                         std::size_t tab_size,
                         std::size_t indent_width,
                         std::size_t caret_line,
                         std::size_t caret_leading_visual_indent,
                         std::vector<IndentGuideRun>* out,
                         const FoldingModel* folding_model = nullptr);

// Returns the leading visual indent count for `line`, expanding tabs to
// `tab_size` cells. Stops at the first non-whitespace character.
std::size_t LeadingVisualIndent(const std::string& line, std::size_t tab_size);

}  // namespace microide::editor
