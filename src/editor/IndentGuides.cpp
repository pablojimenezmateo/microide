#include "editor/IndentGuides.h"

#include "editor/LineIndentScan.h"

#include "editor/FoldingModel.h"
#include "editor/TextLayout.h"
#include "util/StringUtil.h"

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
  (void)AdvanceLeadingIndentOverChunk(line, tab_size, visual);
  return visual;
}

std::size_t LeadingVisualIndent(LineSpan lines, std::size_t line_index, std::size_t tab_size) {
  return MeasureLeadingIndent(lines, line_index, tab_size);
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
              LeadingVisualIndent(lines, fold->opener_line, tab_size);
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

  // Measure each visible row's indent once, then sweep COLUMN BY COLUMN emitting
  // contiguous vertical runs directly.
  //
  // This used to emit one entry per (row, guide column) into a scratch vector and
  // then std::sort it by (column, active, start_row) so the coalescing sweep could
  // merge neighbours. The entry count is rows x indent levels, which on deeply
  // indented content is thousands per frame -- the sort measured 15.6 ms of the
  // 17.0 ms this whole step cost across a 372-frame scroll (51 us per compute),
  // making it the single largest thing in the editor's render path. Walking one
  // column at a time produces the runs already grouped and already in row order,
  // so there is nothing to sort: O(rows x levels) with no comparisons.
  //
  // The emitted SET is unchanged. The order differs only in that a column whose
  // rows include the active guide now yields its runs in row order rather than
  // all-inactive-then-active; runs are disjoint in (column, row) so nothing
  // downstream depends on that.
  thread_local std::vector<std::size_t> leading_scratch;
  leading_scratch.assign(visible_rows.size(), 0);
  std::size_t max_leading = 0;
  for (std::size_t row = 0; row < visible_rows.size(); ++row) {
    const std::size_t line_index = visible_rows[row];
    if (line_index >= lines.size()) {
      continue;
    }
    const std::size_t leading = LeadingVisualIndent(lines, line_index, tab_size);
    leading_scratch[row] = leading;
    max_leading = std::max(max_leading, leading);
  }

  for (std::size_t column = indent_width; column <= max_leading; column += indent_width) {
    bool in_run = false;
    IndentGuideRun run{};
    for (std::size_t row = 0; row < visible_rows.size(); ++row) {
      const bool covered = leading_scratch[row] >= column;
      const bool active =
          covered && visible_rows[row] == caret_line && column == active_column;
      if (covered && in_run && run.active == active && run.end_row + 1 == row) {
        run.end_row = row;
        continue;
      }
      if (in_run) {
        out->push_back(run);
        in_run = false;
      }
      if (covered) {
        run.column = column;
        run.start_row = row;
        run.end_row = row;
        run.active = active;
        in_run = true;
      }
    }
    if (in_run) {
      out->push_back(run);
    }
  }
}

}  // namespace microide::editor
