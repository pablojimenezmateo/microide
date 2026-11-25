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

}

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

  constexpr std::size_t kMaxGuideVisualColumn = 512;
  thread_local std::vector<std::size_t> leading_scratch;
  leading_scratch.assign(visible_rows.size(), 0);
  std::size_t max_leading = 0;
  for (std::size_t row = 0; row < visible_rows.size(); ++row) {
    const std::size_t line_index = visible_rows[row];
    if (line_index >= lines.size()) {
      continue;
    }
    const std::size_t leading =
        std::min(LeadingVisualIndent(lines, line_index, tab_size), kMaxGuideVisualColumn);
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

}
