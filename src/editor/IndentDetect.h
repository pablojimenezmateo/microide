#pragma once

#include <cstddef>

#include "editor/LineSpan.h"

namespace microide::editor {

struct IndentDetection {
  bool soft_tabs = false;
  std::size_t indent_width = 4;
  bool detected = false;
  // Count of non-blank lines actually inspected (stops at `max_inspect_lines`).
  std::size_t non_blank_lines_inspected = 0;
};

// Inspects up to `max_inspect_lines` non-blank lines at the start of `lines`
// and returns a heuristic detection. Tabs majority -> hard tabs; spaces
// majority -> soft tabs with the most-common positive indent step. Ties or
// insufficient signal leave `detected = false` and the defaults intact.
//
// `lines` is a LineSpan, so a live `TextBuffer` can be passed directly (zero-copy
// via LineView) without materializing the whole document with Snapshot()
// (TD-2026-07-17A-003). A `std::vector<std::string>` still converts implicitly.
IndentDetection DetectIndent(LineSpan lines, std::size_t max_inspect_lines = 256);

}  // namespace microide::editor
