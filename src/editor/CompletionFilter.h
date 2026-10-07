#pragma once

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::editor {

// The completion list's local filter: how the word typed since the list opened
// narrows and reorders the candidates the sources returned, without asking them
// again. Shaped after VS Code's suggest filter rather than a path matcher:
//
// - the query must be a case-insensitive SUBSEQUENCE of the candidate's filter
//   text, and its first character must land on a word start (the text's start, the
//   character after a non-alphanumeric, or a camelCase hump). So `gs` finds
//   `getString` and `get_size` but `x` does not find `index`, which is noise in a
//   list a human scans while typing;
// - a contiguous run, a match at a word start and an exact-case character each
//   score, and skipped characters cost a little, so `getS` ranks `getString`
//   above `getSomethingElseString`;
// - an empty query matches everything at score 0, and ranking is stable, so the
//   sources' own order (the server's sortText, LSP before plugin) decides every tie.
//
// Bytes, not code points: a non-ASCII byte matches only itself, which keeps the
// filter allocation-free and is exactly right for identifier text.
inline constexpr int kNoCompletionMatch = INT_MIN;

int CompletionMatchScore(std::string_view candidate, std::string_view query);

// Rank `count` candidates against `query`, writing the indices of those that
// match into `out`, best first, ties in index order. `filter_text_of(i)` returns
// candidate i's filter text as a string_view. `scratch` and `out` are cleared and
// reused, so a steady-state keystroke allocates nothing.
template <typename FilterTextOf>
void RankCompletionCandidates(std::size_t count, std::string_view query,
                              FilterTextOf&& filter_text_of,
                              std::vector<std::pair<int, std::uint32_t>>& scratch,
                              std::vector<std::uint32_t>& out) {
  out.clear();
  if (query.empty()) {
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      out.push_back(static_cast<std::uint32_t>(i));
    }
    return;
  }
  scratch.clear();
  for (std::size_t i = 0; i < count; ++i) {
    const int score = CompletionMatchScore(filter_text_of(i), query);
    if (score != kNoCompletionMatch) {
      scratch.emplace_back(score, static_cast<std::uint32_t>(i));
    }
  }
  std::stable_sort(scratch.begin(), scratch.end(),
                   [](const auto& a, const auto& b) { return a.first > b.first; });
  out.reserve(scratch.size());
  for (const auto& [score, index] : scratch) {
    out.push_back(index);
  }
}

}  // namespace microide::editor
