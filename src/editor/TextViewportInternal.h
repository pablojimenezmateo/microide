#pragma once

// Internal helpers shared between the TextViewport.cpp translation units
// (TextViewport.cpp and TextViewportLanguageBehavior.cpp). Not part of the
// public editor API. The `detail` namespace and the .h naming both signal
// "do not include this from anywhere outside src/editor/TextViewport*.cpp".

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/LineSpan.h"
#include "editor/TextViewport.h"

namespace microide::editor::detail {

// Highlight checkpoint spacing. One SyntaxState is snapshotted every
// `kHighlightCheckpointInterval` lines so resuming highlighting after a jump
// is O(checkpoint-interval) rather than O(line-index). Shared between
// TextViewport.cpp (InvalidateDerivedCaches checkpoint-bookkeeping) and
// TextViewportHighlightCache.cpp (the cache itself).
inline constexpr std::size_t kHighlightCheckpointInterval = 128;

inline bool PositionLess(const TextPosition& lhs, const TextPosition& rhs) {
  if (lhs.line != rhs.line) {
    return lhs.line < rhs.line;
  }
  return lhs.column < rhs.column;
}

inline std::optional<SelectionRange> SelectionRangeForSecondaryCaret(
    const TextPosition& position,
    const std::optional<TextPosition>& selection_anchor) {
  if (!selection_anchor.has_value()) {
    return std::nullopt;
  }
  if (selection_anchor->line == position.line && selection_anchor->column == position.column) {
    return std::nullopt;
  }
  if (PositionLess(*selection_anchor, position)) {
    return SelectionRange{*selection_anchor, position};
  }
  return SelectionRange{position, *selection_anchor};
}

inline TextPosition RangeEndExclusive(const SelectionRange& r) {
  return PositionLess(r.start, r.end) ? r.end : r.start;
}

inline bool ValidateRangeColumns(LineSpan lines, const SelectionRange& n) {
  if (n.start.line >= lines.size() || n.end.line >= lines.size()) {
    return false;
  }
  if (n.start.column > lines[n.start.line].size() ||
      n.end.column > lines[n.end.line].size()) {
    return false;
  }
  return true;
}

inline std::string TextBetweenLines(LineSpan lines, const SelectionRange& n) {
  const auto& a = n.start;
  const auto& b = n.end;
  if (a.line == b.line) {
    return std::string(lines[a.line].substr(a.column, b.column - a.column));
  }
  std::string out;
  out += lines[a.line].substr(a.column);
  out.push_back('\n');
  for (std::size_t i = a.line + 1; i < b.line; ++i) {
    out += lines[i];
    out.push_back('\n');
  }
  out += lines[b.line].substr(0, b.column);
  return out;
}

// Maps a caret position forward across one applied edit that replaced the
// (normalized) range [removed_start, removed_end) with `replacement`. The
// multi-caret pipelines walk carets high-to-low, so a caret recorded earlier
// always sits at or after a later (lower) edit; remapping keeps positions
// correct when several carets share a line (without it, the higher carets are
// left stale by the byte/line counts inserted below them).
inline TextPosition RemapPositionAfterReplace(TextPosition position,
                                              TextPosition removed_start,
                                              TextPosition removed_end,
                                              std::string_view replacement) {
  std::size_t inserted_newlines = 0;
  std::size_t last_segment_cols = 0;
  for (const char ch : replacement) {
    if (ch == '\n') {
      ++inserted_newlines;
      last_segment_cols = 0;
    } else {
      ++last_segment_cols;
    }
  }

  // Positions strictly before the end of the removed range are unaffected.
  if (PositionLess(position, removed_end)) {
    return position;
  }

  if (position.line == removed_end.line) {
    TextPosition result;
    result.line = removed_start.line + inserted_newlines;
    const std::size_t tail = position.column - removed_end.column;
    result.column = inserted_newlines == 0
                        ? removed_start.column + last_segment_cols + tail
                        : last_segment_cols + tail;
    return result;
  }

  const std::ptrdiff_t line_delta =
      static_cast<std::ptrdiff_t>(inserted_newlines) -
      static_cast<std::ptrdiff_t>(removed_end.line - removed_start.line);
  TextPosition result = position;
  result.line = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(position.line) + line_delta);
  return result;
}

}  // namespace microide::editor::detail
