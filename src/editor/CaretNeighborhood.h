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

// Bytes read before the caret. Four is what the clamp needs (a code-point start is
// at most three bytes back, and `prev` sits one before that); eight leaves the
// backward code-point walk the same room, and caps it the way
// `TextLayout::ClampTextColumn` already caps its own: inside a malformed run
// longer than this the answer stays a valid position -- inside the line, no
// greater than asked, never splitting a well-formed code point -- it is simply
// nearer the caret.
inline constexpr std::size_t kCaretNeighborhoodLookbehindBytes = 8;

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
  const std::size_t length = lines.LineLength(line);
  result.line_length = length;
  const std::size_t at = std::min(column, length);
  const std::size_t window_start =
      at >= kCaretNeighborhoodLookbehindBytes ? at - kCaretNeighborhoodLookbehindBytes : 0;
  std::string scratch;
  // `+ 1` so the byte AT the caret is in the window when there is one; LineWindow
  // clamps to the line, so at end-of-line the window simply ends there.
  const std::string_view window =
      lines.LineWindow(line, window_start, (at - window_start) + 1, scratch);
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
    std::size_t local_prev = local_clamped - 1;
    while (local_prev > 0 &&
           (static_cast<unsigned char>(window[local_prev]) & 0xC0u) == 0x80u) {
      --local_prev;
    }
    result.prev_column = window_start + local_prev;
    result.prev_char = window[local_prev];
  }
  return result;
}

}  // namespace microide::editor
