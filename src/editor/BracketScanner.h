#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace microide::editor {

class TextViewport;

struct BracketMatchPair {
  std::size_t open_line = 0;
  std::size_t open_column = 0;
  std::size_t close_line = 0;
  std::size_t close_column = 0;
  // True if the originating caret was adjacent to the open bracket; false if
  // adjacent to the close bracket. Useful for differential paint emphasis.
  bool caret_at_opener = false;
};

struct BracketDescriptor {
  char open;
  char close;
};

// Single-character bracket pair set used by the default scanner. Higher-fidelity
// matching that consults the language contract may pass a custom span.
inline constexpr BracketDescriptor kDefaultBracketSet[] = {
    {'{', '}'},
    {'(', ')'},
    {'[', ']'},
};

// Returns the matching bracket pair when the caret is adjacent to a bracket
// character, scanning forward from open and backward from close. Returns
// nullopt when no adjacent bracket exists or when the bracket is unbalanced
// within `max_lines_each_side`.
std::optional<BracketMatchPair> FindBracketMatch(const TextViewport& viewport,
                                                 std::size_t caret_line,
                                                 std::size_t caret_column,
                                                 std::size_t max_lines_each_side = 2000);

// Lower-level scanner for tests. When `syntax_viewport` is non-null and line
// indices align with `lines`, bracket characters classified as String or Comment
// via `HighlightedLineTokens` are ignored (same rule as `FoldingModel`).
std::optional<BracketMatchPair> FindBracketMatchInLines(
    const std::vector<std::string_view>& lines,
    std::size_t caret_line,
    std::size_t caret_column,
    std::size_t max_lines_each_side,
    const TextViewport* syntax_viewport = nullptr);

}  // namespace microide::editor
