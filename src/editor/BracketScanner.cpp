#include "editor/BracketScanner.h"

#include "editor/TextViewport.h"

namespace microide::editor {

namespace {

constexpr bool IsOpener(char c) {
  return c == '{' || c == '(' || c == '[';
}

constexpr bool IsCloser(char c) {
  return c == '}' || c == ')' || c == ']';
}

constexpr char Opposite(char c) {
  switch (c) {
    case '{': return '}';
    case '}': return '{';
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
  }
  return 0;
}

bool MatchForwardFromOpener(const std::vector<std::string_view>& lines,
                            std::size_t open_line,
                            std::size_t open_col,
                            std::size_t max_lines_each_side,
                            BracketMatchPair* out) {
  const char open_ch = lines[open_line][open_col];
  const char close_ch = Opposite(open_ch);
  int depth = 1;
  std::size_t end_line = open_line + max_lines_each_side;
  if (end_line > lines.size()) end_line = lines.size();

  std::size_t line = open_line;
  std::size_t col = open_col + 1;
  while (line < end_line) {
    std::string_view text = lines[line];
    while (col < text.size()) {
      char c = text[col];
      if (c == open_ch) {
        ++depth;
      } else if (c == close_ch) {
        --depth;
        if (depth == 0) {
          out->open_line = open_line;
          out->open_column = open_col;
          out->close_line = line;
          out->close_column = col;
          return true;
        }
      }
      ++col;
    }
    ++line;
    col = 0;
  }
  return false;
}

bool MatchBackwardFromCloser(const std::vector<std::string_view>& lines,
                             std::size_t close_line,
                             std::size_t close_col,
                             std::size_t max_lines_each_side,
                             BracketMatchPair* out) {
  const char close_ch = lines[close_line][close_col];
  const char open_ch = Opposite(close_ch);
  int depth = 1;

  std::size_t bottom = close_line >= max_lines_each_side ? close_line - max_lines_each_side : 0;

  // First scan the closer's own line to the left of close_col.
  if (close_col > 0) {
    std::string_view text = lines[close_line];
    std::size_t col = close_col;
    while (col > 0) {
      --col;
      char c = text[col];
      if (c == close_ch) {
        ++depth;
      } else if (c == open_ch) {
        --depth;
        if (depth == 0) {
          out->open_line = close_line;
          out->open_column = col;
          out->close_line = close_line;
          out->close_column = close_col;
          return true;
        }
      }
    }
  }

  // Then scan earlier lines, right-to-left.
  std::size_t line = close_line;
  while (line > bottom) {
    --line;
    std::string_view text = lines[line];
    std::size_t col = text.size();
    while (col > 0) {
      --col;
      char c = text[col];
      if (c == close_ch) {
        ++depth;
      } else if (c == open_ch) {
        --depth;
        if (depth == 0) {
          out->open_line = line;
          out->open_column = col;
          out->close_line = close_line;
          out->close_column = close_col;
          return true;
        }
      }
    }
  }
  return false;
}

}  // namespace

std::optional<BracketMatchPair> FindBracketMatchInLines(
    const std::vector<std::string_view>& lines,
    std::size_t caret_line,
    std::size_t caret_column,
    std::size_t max_lines_each_side) {
  if (caret_line >= lines.size()) return std::nullopt;
  std::string_view current = lines[caret_line];

  // Try character at the caret first (caret is to the left of position).
  auto try_at = [&](std::size_t col, bool prefer_opener) -> std::optional<BracketMatchPair> {
    if (col >= current.size()) return std::nullopt;
    char c = current[col];
    BracketMatchPair pair;
    if (IsOpener(c)) {
      if (MatchForwardFromOpener(lines, caret_line, col, max_lines_each_side, &pair)) {
        pair.caret_at_opener = prefer_opener;
        return pair;
      }
    } else if (IsCloser(c)) {
      if (MatchBackwardFromCloser(lines, caret_line, col, max_lines_each_side, &pair)) {
        pair.caret_at_opener = false;
        return pair;
      }
    }
    return std::nullopt;
  };

  if (caret_column < current.size()) {
    if (auto p = try_at(caret_column, /*prefer_opener=*/true)) return p;
  }
  if (caret_column > 0) {
    if (auto p = try_at(caret_column - 1, /*prefer_opener=*/false)) return p;
  }
  return std::nullopt;
}

std::optional<BracketMatchPair> FindBracketMatch(const TextViewport& viewport,
                                                 std::size_t caret_line,
                                                 std::size_t caret_column,
                                                 std::size_t max_lines_each_side) {
  const auto& lines = viewport.lines();
  // Reuse thread-local storage so per-frame bracket matching does not hit the
  // heap when the file is large (typing/scrolling only reallocates on growth).
  thread_local std::vector<std::string_view> views;
  views.resize(lines.size());
  for (std::size_t i = 0; i < lines.size(); ++i) {
    views[i] = lines[i];
  }
  return FindBracketMatchInLines(views, caret_line, caret_column, max_lines_each_side);
}

}  // namespace microide::editor
