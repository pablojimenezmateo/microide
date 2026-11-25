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
  bool caret_at_opener = false;
};

struct BracketDescriptor {
  char open;
  char close;
};

inline constexpr BracketDescriptor kDefaultBracketSet[] = {
    {'{', '}'},
    {'(', ')'},
    {'[', ']'},
};

inline constexpr std::size_t kMaxBracketMatchScanBytes = 512u * 1024u;

std::optional<BracketMatchPair> FindBracketMatch(const TextViewport& viewport,
                                                 std::size_t caret_line,
                                                 std::size_t caret_column,
                                                 std::size_t max_lines_each_side = 2000);

std::optional<BracketMatchPair> FindBracketMatchInLines(
    const std::vector<std::string_view>& lines,
    std::size_t caret_line,
    std::size_t caret_column,
    std::size_t max_lines_each_side,
    const TextViewport* syntax_viewport = nullptr);

}
