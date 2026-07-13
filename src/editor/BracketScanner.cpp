#include "editor/BracketScanner.h"

#include <algorithm>

#include "editor/SyntaxHighlighter.h"
#include "editor/TextViewport.h"

namespace microide::editor {

namespace {

bool IsBracketScanSuppressed(const TextViewport* viewport,
                             std::size_t line_index,
                             std::size_t column) {
  if (viewport == nullptr) {
    return false;
  }
  const auto& tokens = viewport->HighlightedLineTokens(line_index);
  if (tokens.empty() || column >= tokens.size()) {
    return false;
  }
  const SyntaxTokenKind kind = tokens[column];
  return kind == SyntaxTokenKind::String || kind == SyntaxTokenKind::Comment;
}

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
                            const TextViewport* syntax_viewport,
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
      if (IsBracketScanSuppressed(syntax_viewport, line, col)) {
        ++col;
        continue;
      }
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
                             const TextViewport* syntax_viewport,
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
      if (IsBracketScanSuppressed(syntax_viewport, close_line, col)) {
        continue;
      }
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
      if (IsBracketScanSuppressed(syntax_viewport, line, col)) {
        continue;
      }
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
    std::size_t max_lines_each_side,
    const TextViewport* syntax_viewport) {
  if (caret_line >= lines.size()) return std::nullopt;
  std::string_view current = lines[caret_line];

  // Try character at the caret first (caret is to the left of position).
  // `caret_at_opener` follows the matched character, not which probe found it:
  // the caret is adjacent to the open bracket whenever the resolved character is
  // an opener (including the left-adjacent `foo(|bar)` case), and adjacent to the
  // close bracket when it is a closer.
  auto try_at = [&](std::size_t col) -> std::optional<BracketMatchPair> {
    if (col >= current.size()) return std::nullopt;
    if (IsBracketScanSuppressed(syntax_viewport, caret_line, col)) {
      return std::nullopt;
    }
    char c = current[col];
    BracketMatchPair pair;
    if (IsOpener(c)) {
      if (MatchForwardFromOpener(lines, caret_line, col, max_lines_each_side, syntax_viewport,
                                  &pair)) {
        pair.caret_at_opener = true;
        return pair;
      }
    } else if (IsCloser(c)) {
      if (MatchBackwardFromCloser(lines, caret_line, col, max_lines_each_side, syntax_viewport,
                                   &pair)) {
        pair.caret_at_opener = false;
        return pair;
      }
    }
    return std::nullopt;
  };

  if (caret_column < current.size()) {
    if (auto p = try_at(caret_column)) return p;
  }
  if (caret_column > 0) {
    if (auto p = try_at(caret_column - 1)) return p;
  }
  return std::nullopt;
}

std::optional<BracketMatchPair> FindBracketMatch(const TextViewport& viewport,
                                                 std::size_t caret_line,
                                                 std::size_t caret_column,
                                                 std::size_t max_lines_each_side) {
  const auto& lines = viewport.lines();
  const std::size_t line_count = lines.size();
  // Only the window [caret_line - max, caret_line + max] is ever scanned: the
  // forward/backward matchers never look past it and the caret probes its own
  // line. Materializing a LineView for every line in the buffer each frame was
  // O(file) work for a bounded scan. Size the thread-local vector to the line
  // count so absolute line indexing stays valid (the empty-view fill is a cheap
  // memory write versus a per-line LineView call), then fill only the window.
  // Out-of-window slots are never dereferenced.
  thread_local std::vector<std::string_view> views;
  views.assign(line_count, std::string_view{});
  if (caret_line < line_count) {
    const std::size_t lo =
        caret_line > max_lines_each_side ? caret_line - max_lines_each_side : 0;
    const std::size_t hi = std::min(line_count, caret_line + max_lines_each_side + 1);
    for (std::size_t i = lo; i < hi; ++i) {
      views[i] = lines.LineView(i);
    }
  }
  return FindBracketMatchInLines(views, caret_line, caret_column, max_lines_each_side, &viewport);
}

}  // namespace microide::editor
