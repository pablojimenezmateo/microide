#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "editor/LineSpan.h"

namespace microide::editor {

class FoldingModel;

struct IndentGuideRun {
  std::size_t column = 0;
  std::size_t start_row = 0;
  std::size_t end_row = 0;
  bool active = false;
};

void ComputeIndentGuides(LineSpan lines,
                         const std::vector<std::size_t>& visible_rows,
                         std::size_t tab_size,
                         std::size_t indent_width,
                         std::size_t caret_line,
                         std::size_t caret_leading_visual_indent,
                         std::vector<IndentGuideRun>* out,
                         const FoldingModel* folding_model = nullptr);

std::size_t LeadingVisualIndent(std::string_view line, std::size_t tab_size);

std::size_t LeadingVisualIndent(LineSpan lines, std::size_t line_index, std::size_t tab_size);

}
