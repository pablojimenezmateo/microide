#pragma once

#include <cstddef>
#include <optional>

#include "editor/LineSpan.h"

namespace microide::editor {

struct IndentDetection {
  // Set when the buffer says which style it uses: true = spaces, false = tabs.
  // Empty when there is no indented line, or tabs and spaces tie.
  std::optional<bool> soft_tabs;
  // The indentation step, set only for a space-indented buffer whose consecutive
  // lines step by a size in [2, 8]. A tab-indented buffer keeps the caller's tab
  // size (the buffer cannot say how wide a tab is).
  std::optional<std::size_t> indent_width;
  // Count of non-blank lines actually inspected (stops at `max_inspect_lines`).
  std::size_t non_blank_lines_inspected = 0;

  bool detected() const { return soft_tabs.has_value() || indent_width.has_value(); }
};

// VS Code's guessIndentation over the first `max_inspect_lines` non-blank lines:
// the style is the majority of tab- vs space-indented lines; the width is the
// most frequent change in leading spaces between consecutive content lines
// (both directions), candidates ordered 2,4,6,8,3,5,7, where 2 beats 4 whenever
// 2-steps are at least half as common as 4-steps (a 2-space file dedents by two
// levels at once as often as not, and its continuation lines sit at +4). A line
// aligned to a comma-ended predecessor is not evidence of anything.
//
// `lines` is a LineSpan, so a live `TextBuffer` can be passed directly (zero-copy
// via LineView) without materializing the whole document with Snapshot()
// (TD-2026-07-17A-003). A `std::vector<std::string>` still converts implicitly.
IndentDetection DetectIndent(LineSpan lines, std::size_t max_inspect_lines = 1024);

}  // namespace microide::editor
