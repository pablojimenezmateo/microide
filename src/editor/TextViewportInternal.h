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

// Order secondary carets by their caret position. Shared by the
// AddSecondaryCaret / AddSecondaryCaretWithRange / DedupeSecondaryCaretsAgainstPrimary
// sort sites so they cannot drift apart.
inline bool SecondaryCaretPositionLess(const TextViewportUndoHistory::SecondaryCaret& lhs,
                                       const TextViewportUndoHistory::SecondaryCaret& rhs) {
  return PositionLess(lhs.position, rhs.position);
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

// Newline / trailing-column counts of a replacement string. Computed once per
// edit so the per-caret remap below is a branch-only update instead of
// re-scanning the replacement for every previously-placed caret (the multi-caret
// fan-out remaps O(k) prior carets per applied edit, so the rescan was
// O(k^2 * |replacement|)).
struct ReplacementShape {
  std::size_t inserted_newlines = 0;
  std::size_t last_segment_cols = 0;
};

inline ReplacementShape ComputeReplacementShape(std::string_view replacement) {
  ReplacementShape shape;
  for (const char ch : replacement) {
    if (ch == '\n') {
      ++shape.inserted_newlines;
      shape.last_segment_cols = 0;
    } else {
      ++shape.last_segment_cols;
    }
  }
  return shape;
}

// Maps a caret position forward across one applied edit that replaced the
// (normalized) range [removed_start, removed_end) with a replacement whose
// newline/column shape is `shape`. The multi-caret pipelines walk carets
// high-to-low, so a caret recorded earlier always sits at or after a later
// (lower) edit; remapping keeps positions correct when several carets share a
// line (without it, the higher carets are left stale by the byte/line counts
// inserted below them).
inline TextPosition RemapPositionAfterReplace(TextPosition position,
                                              TextPosition removed_start,
                                              TextPosition removed_end,
                                              const ReplacementShape& shape) {
  // Positions strictly before the end of the removed range are unaffected.
  if (PositionLess(position, removed_end)) {
    return position;
  }

  if (position.line == removed_end.line) {
    TextPosition result;
    result.line = removed_start.line + shape.inserted_newlines;
    const std::size_t tail = position.column - removed_end.column;
    result.column = shape.inserted_newlines == 0
                        ? removed_start.column + shape.last_segment_cols + tail
                        : shape.last_segment_cols + tail;
    return result;
  }

  const std::ptrdiff_t line_delta =
      static_cast<std::ptrdiff_t>(shape.inserted_newlines) -
      static_cast<std::ptrdiff_t>(removed_end.line - removed_start.line);
  TextPosition result = position;
  result.line = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(position.line) + line_delta);
  return result;
}

}  // namespace microide::editor::detail
