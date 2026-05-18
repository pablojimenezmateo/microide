#pragma once

// Internal helpers shared between the TextViewport.cpp translation units
// (TextViewport.cpp and TextViewportLanguageBehavior.cpp). Not part of the
// public editor API. The `detail` namespace and the .h naming both signal
// "do not include this from anywhere outside src/editor/TextViewport*.cpp".

#include <optional>
#include <string>
#include <vector>

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

inline bool ValidateRangeColumns(const std::vector<std::string>& lines,
                                  const SelectionRange& n) {
  if (n.start.line >= lines.size() || n.end.line >= lines.size()) {
    return false;
  }
  if (n.start.column > lines[n.start.line].size() ||
      n.end.column > lines[n.end.line].size()) {
    return false;
  }
  return true;
}

inline std::string TextBetweenLines(const std::vector<std::string>& lines,
                                     const SelectionRange& n) {
  const auto& a = n.start;
  const auto& b = n.end;
  if (a.line == b.line) {
    return lines[a.line].substr(a.column, b.column - a.column);
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

}  // namespace microide::editor::detail
