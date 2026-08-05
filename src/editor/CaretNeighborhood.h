#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "editor/LineSpan.h"
#include "editor/TextLayout.h"

namespace microide::editor {

// Everything the caret-local editing heuristics actually consult about a
// position, read without touching more than a handful of bytes of the line.
//
// Auto-close, skip-over-close, brace-split and the multi-caret pair fan-out all
// decide from the byte on either side of the caret. Each of them used to start by
// materializing the whole line (`lines[line]`), which on a file with no line
// breaks in it is megabytes per inserted character, on the keystroke path
// (TD-2026-08-05-133). Route new caret-local heuristics through this rather than
// adding another whole-line read.
struct CaretNeighborhood {
  std::size_t line_length = 0;
  // `column` rounded down to a code-point start and clamped to the line -- the
  // same answer `TextLayout::ClampTextColumn(whole_line, column)` gives.
  std::size_t clamped_column = 0;
  bool at_end = false;    // clamped_column == line_length; `next` is unset
  char next = '\0';       // byte at clamped_column
  bool has_prev = false;  // clamped_column > 0
  char prev = '\0';       // byte at clamped_column - 1
};

// Reads the line's LENGTH (no text) plus at most five bytes ending at `column`.
// Five is the bound the clamp forces: UTF-8 is self-synchronizing, so the code
// point containing `column` starts at most three bytes back, and `prev` sits one
// byte before that start.
inline CaretNeighborhood ReadCaretNeighborhood(LineSpan lines, std::size_t line,
                                               std::size_t column) {
  CaretNeighborhood result;
  if (line >= lines.size()) {
    return result;
  }
  result.line_length = lines.LineLength(line);
  std::string scratch;
  if (column >= result.line_length) {
    // ClampTextColumn's own answer for a column at or past the end, without
    // needing the line to produce it.
    result.clamped_column = result.line_length;
    result.at_end = true;
    if (result.line_length > 0) {
      const std::string_view tail = lines.LineWindow(line, result.line_length - 1, 1, scratch);
      result.has_prev = !tail.empty();
      result.prev = result.has_prev ? tail.front() : '\0';
    }
    return result;
  }

  const std::size_t window_start = column >= 4 ? column - 4 : 0;
  const std::string_view window =
      lines.LineWindow(line, window_start, (column - window_start) + 1, scratch);
  const std::size_t local = TextLayout::ClampTextColumn(window, column - window_start);
  result.clamped_column = window_start + local;
  result.next = window[local];
  // `local > 0` and `clamped_column > 0` are the same condition here: with
  // window_start > 0 the requested local column is exactly 4 and the clamp steps
  // back at most 3, so `local` cannot reach 0; with window_start == 0 the two
  // columns are equal. Both are written out so the window sizing above stays
  // load-bearing rather than incidental.
  result.has_prev = result.clamped_column > 0 && local > 0;
  if (result.has_prev) {
    result.prev = window[local - 1];
  }
  return result;
}

}  // namespace microide::editor
