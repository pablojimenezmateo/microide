#include "editor/IndentGuides.h"

#include "editor/FoldingModel.h"

#include <algorithm>
#include <climits>
#include <cstdint>

namespace microide::editor {

namespace {

std::size_t ActiveGuideColumnFromLeading(std::size_t caret_leading_visual_indent,
                                          std::size_t indent_width) {
  std::size_t active_column = SIZE_MAX;
  if (caret_leading_visual_indent >= indent_width) {
    const std::size_t rounded =
        (caret_leading_visual_indent / indent_width) * indent_width;
    active_column =
        rounded == caret_leading_visual_indent ? rounded - indent_width : rounded;
  }
  return active_column;
}

}  // namespace

std::size_t LeadingVisualIndent(std::string_view line, std::size_t tab_size) {
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

void ComputeIndentGuides(LineSpan lines,
                         const std::vector<std::size_t>& visible_rows,
                         std::size_t tab_size,
                         std::size_t indent_width,
                         std::size_t caret_line,
                         std::size_t caret_leading_visual_indent,
                         std::vector<IndentGuideRun>* out,
                         const FoldingModel* folding_model) {
  if (out == nullptr) return;
  out->clear();
  if (indent_width == 0) return;

  // Compute the caret's "active" guide column: the immediate parent block.
  std::size_t active_column = SIZE_MAX;
  if (caret_line != SIZE_MAX) {
    if (folding_model != nullptr &&
        caret_line < lines.size()) {
      if (const auto fold = folding_model->InnermostFoldContaining(caret_line)) {
        if (caret_line > fold->opener_line && fold->opener_line < lines.size()) {
          const std::size_t opener_lead =
              LeadingVisualIndent(lines[fold->opener_line], tab_size);
          if (opener_lead >= indent_width) {
            active_column = (opener_lead / indent_width) * indent_width;
          }
        }
      }
    }
    if (active_column == SIZE_MAX) {
      active_column = ActiveGuideColumnFromLeading(caret_leading_visual_indent, indent_width);
    }
  }

  // Pass 1: emit per-row guides into a scratch vector, then coalesce vertical
  // runs of identical (column, active) values into single segments. The
  // renderer paints a rectangle per IndentGuideRun, so collapsing 50 rows ×
  // 8 indent levels from 400 single-row segments into ~8 vertical segments
  // is a 50× reduction in render-loop work.
  thread_local std::vector<IndentGuideRun> per_row_scratch;
  per_row_scratch.clear();

  for (std::size_t row = 0; row < visible_rows.size(); ++row) {
    const std::size_t line_index = visible_rows[row];
    if (line_index >= lines.size()) continue;
    const std::size_t leading = LeadingVisualIndent(lines[line_index], tab_size);
    if (leading == 0) continue;
    for (std::size_t column = indent_width; column <= leading; column += indent_width) {
      IndentGuideRun guide;
      guide.column = column;
      guide.start_row = row;
      guide.end_row = row;
      guide.active = (line_index == caret_line && column == active_column);
      per_row_scratch.push_back(guide);
    }
  }

  // Pass 2: coalesce. Sort by (column, active, start_row) so adjacent rows with
  // identical column/active merge naturally. Then sweep to merge contiguous
  // runs (end_row + 1 == next.start_row).
  std::sort(per_row_scratch.begin(), per_row_scratch.end(),
            [](const IndentGuideRun& a, const IndentGuideRun& b) {
              if (a.column != b.column) return a.column < b.column;
              if (a.active != b.active) return a.active < b.active;
              return a.start_row < b.start_row;
            });
  out->reserve(per_row_scratch.size());
  for (const IndentGuideRun& guide : per_row_scratch) {
    if (!out->empty()) {
      IndentGuideRun& back = out->back();
      if (back.column == guide.column && back.active == guide.active &&
          back.end_row + 1 == guide.start_row) {
        back.end_row = guide.end_row;
        continue;
      }
    }
    out->push_back(guide);
  }
}

}  // namespace microide::editor
