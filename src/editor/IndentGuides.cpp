#include "editor/IndentGuides.h"

#include <algorithm>
#include <climits>
#include <cstdint>

namespace microide::editor {

std::size_t LeadingVisualIndent(const std::string& line, std::size_t tab_size) {
  std::size_t visual = 0;
  for (char c : line) {
    if (c == ' ') {
      ++visual;
    } else if (c == '\t') {
      const std::size_t step = tab_size == 0 ? 1 : tab_size;
      visual += step - (visual % step);
    } else {
      break;
    }
  }
  return visual;
}

void ComputeIndentGuides(const std::vector<std::string>& lines,
                         const std::vector<std::size_t>& visible_rows,
                         std::size_t tab_size,
                         std::size_t indent_width,
                         std::size_t caret_line,
                         std::size_t caret_leading_visual_indent,
                         std::vector<IndentGuideRun>* out) {
  if (out == nullptr) return;
  out->clear();
  if (indent_width == 0) return;

  // Compute the caret's "active" guide column: the immediate parent block.
  std::size_t active_column = SIZE_MAX;
  if (caret_line != SIZE_MAX && caret_leading_visual_indent >= indent_width) {
    const std::size_t rounded =
        (caret_leading_visual_indent / indent_width) * indent_width;
    active_column =
        rounded == caret_leading_visual_indent ? rounded - indent_width : rounded;
  }

  for (std::size_t row = 0; row < visible_rows.size(); ++row) {
    const std::size_t line_index = visible_rows[row];
    if (line_index >= lines.size()) continue;
    const std::size_t leading = LeadingVisualIndent(lines[line_index], tab_size);
    if (leading == 0) continue;
    // Emit a guide at every indent step that the line participates in: columns
    // indent_width, 2*indent_width, ..., up to and including leading. The
    // guide at column == leading marks the line's own indent boundary;
    // shallower guides mark the parent blocks the line is nested inside.
    for (std::size_t column = indent_width; column <= leading; column += indent_width) {
      IndentGuideRun guide;
      guide.column = column;
      guide.start_row = row;
      guide.end_row = row;
      guide.active = (line_index == caret_line && column == active_column);
      out->push_back(guide);
    }
  }
}

}  // namespace microide::editor
