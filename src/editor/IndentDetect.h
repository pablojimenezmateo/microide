#pragma once

#include <cstddef>
#include <optional>

#include "editor/LineSpan.h"

namespace microide::editor {

struct IndentDetection {
  std::optional<bool> soft_tabs;
  std::optional<std::size_t> indent_width;
  std::size_t non_blank_lines_inspected = 0;

  bool detected() const { return soft_tabs.has_value() || indent_width.has_value(); }
};

IndentDetection DetectIndent(LineSpan lines, std::size_t max_inspect_lines = 1024);

}
