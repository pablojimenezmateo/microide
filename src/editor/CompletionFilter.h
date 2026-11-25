#pragma once

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::editor {

inline constexpr int kNoCompletionMatch = INT_MIN;

int CompletionMatchScore(std::string_view candidate, std::string_view query);

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

}
