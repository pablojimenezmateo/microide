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
  // Start of the code point before `clamped_column` -- what
  // `TextLayout::PreviousTextColumn` answers -- and the byte there. Both unset
  // when `has_prev` is false.
  std::size_t prev_column = 0;
  char prev_char = '\0';
};

// Bytes read before the caret. Seven is what the clamp and `prev` need together
// (the covering lead is at most three bytes back, and the unit before it starts
// at most four bytes before that); eight covers it, so the answers equal
// `TextLayout::ClampTextColumn` / `PreviousTextColumn` over the whole line on
// any bytes, malformed included.
inline constexpr std::size_t kCaretNeighborhoodLookbehindBytes = 8;
// Bytes read from the caret on: the byte there plus the three continuation bytes
// a sequence starting before it can still need.
inline constexpr std::size_t kCaretNeighborhoodLookaheadBytes = 4;

// Reads the line's LENGTH (no text) plus a window of at most a dozen bytes
// around `column`. The clamp is exact against the forward tiling on any bytes
// (TextLayout::ClampTextColumn), so it must see the whole sequence of the one
// lead that could cover the caret: up to three bytes back and, to check its
// continuation bytes, up to three bytes past the caret.
inline CaretNeighborhood ReadCaretNeighborhood(LineSpan lines, std::size_t line,
                                               std::size_t column) {
  CaretNeighborhood result;
  if (line >= lines.size()) {
    return result;
  }
  const std::size_t length = lines.LineLength(line);
  result.line_length = length;
  const std::size_t at = std::min(column, length);
  const std::size_t window_start =
      at >= kCaretNeighborhoodLookbehindBytes ? at - kCaretNeighborhoodLookbehindBytes : 0;
  std::string scratch;
  // The byte AT the caret plus the three after it: a lead up to three bytes
  // behind the caret covers it only when its continuation bytes are really there,
  // and a window cut at the caret would read that lead as truncated. LineWindow
  // clamps to the line, so at end-of-line the window simply ends there.
  const std::string_view window =
      lines.LineWindow(line, window_start, (at - window_start) + kCaretNeighborhoodLookaheadBytes,
                       scratch);
  const std::size_t local_at = at - window_start;
  // `at >= length` is ClampTextColumn's own end-of-line answer, reached without
  // needing the line to produce it.
  const std::size_t local_clamped =
      at >= length ? local_at : TextLayout::ClampTextColumn(window, local_at);
  result.clamped_column = window_start + local_clamped;
  result.at_end = result.clamped_column >= length;
  if (!result.at_end) {
    result.next = window[local_clamped];
  }
  // `local_clamped > 0` and `clamped_column > 0` are the same condition: with
  // window_start > 0 the clamp steps back at most three from a local column of
  // eight, and with window_start == 0 the two columns are equal. Both are written
  // out so the window sizing above stays load-bearing rather than incidental.
  result.has_prev = result.clamped_column > 0 && local_clamped > 0;
  if (result.has_prev) {
    result.prev = window[local_clamped - 1];
    const std::size_t local_prev = TextLayout::PreviousTextColumn(window, local_clamped);
    result.prev_column = window_start + local_prev;
    result.prev_char = window[local_prev];
  }
  return result;
}

}  // namespace microide::editor
