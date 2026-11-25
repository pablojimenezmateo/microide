#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "editor/LineSpan.h"
#include "editor/TextLayout.h"

namespace microide::editor {

struct CaretNeighborhood {
  std::size_t line_length = 0;
  std::size_t clamped_column = 0;
  bool at_end = false;
  char next = '\0';
  bool has_prev = false;
  char prev = '\0';
  std::size_t prev_column = 0;
  char prev_char = '\0';
};

inline constexpr std::size_t kCaretNeighborhoodLookbehindBytes = 8;
inline constexpr std::size_t kCaretNeighborhoodLookaheadBytes = 4;

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
  const std::string_view window =
      lines.LineWindow(line, window_start, (at - window_start) + kCaretNeighborhoodLookaheadBytes,
                       scratch);
  const std::size_t local_at = at - window_start;
  const std::size_t local_clamped =
      at >= length ? local_at : TextLayout::ClampTextColumn(window, local_at);
  result.clamped_column = window_start + local_clamped;
  result.at_end = result.clamped_column >= length;
  if (!result.at_end) {
    result.next = window[local_clamped];
  }
  result.has_prev = result.clamped_column > 0 && local_clamped > 0;
  if (result.has_prev) {
    result.prev = window[local_clamped - 1];
    const std::size_t local_prev = TextLayout::PreviousTextColumn(window, local_clamped);
    result.prev_column = window_start + local_prev;
    result.prev_char = window[local_prev];
  }
  return result;
}

}
