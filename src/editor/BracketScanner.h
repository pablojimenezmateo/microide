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

// Declared cap on how many bytes one bracket-match scan may read.
//
// `max_lines_each_side` bounds the scan in LINES, which is only a bound on work
// if lines have a bounded length. On a file with no line breaks in it a
// two-thousand-line window is the whole document, so pressing End next to the
// closing bracket and then any arrow key made every frame re-read megabytes —
// measured at 1.7 ms for a single scan on a 2 MiB line, per caret move.
//
// 512 KiB is chosen against the widest realistic window this could be asked for:
// the 50k-line C++ perf fixture averages 163 bytes a line (deliberately wide;
// this repo's own sources average 43), so its full 2000-line window is ~326 KB.
// The cap is above that with headroom, so no file whose lines are of ordinary
// length can reach it, and the pathological shape stops at a quarter of itself.
//
// Reaching it means the match is reported as not found, exactly as an unbalanced
// bracket within the line window already is — the same degradation mature editors
// apply to bracket matching on very long lines.
inline constexpr std::size_t kMaxBracketMatchScanBytes = 512u * 1024u;

// Returns the matching bracket pair when the caret is adjacent to a bracket
// character, scanning forward from open and backward from close. Returns
// nullopt when no adjacent bracket exists, when the bracket is unbalanced
// within `max_lines_each_side`, or when the scan reaches
// `kMaxBracketMatchScanBytes`.
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
