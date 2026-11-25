#include "editor/BracketScanner.h"

#include <algorithm>

#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/SyntaxHighlighter.h"
#include "editor/TextViewport.h"
#include "util/PerformanceCounters.h"

namespace microide::editor {

namespace {

struct WindowLines {
  const std::vector<std::string_view>* slice = nullptr;
  std::size_t base = 0;

  std::string_view operator[](std::size_t absolute_line) const {
    return (*slice)[absolute_line - base];
  }
  std::size_t size() const { return base + slice->size(); }
};

class LineSuppressionCursor {
 public:
  explicit LineSuppressionCursor(const TextViewport* viewport) : viewport_(viewport) {}

  bool SuppressedAt(std::size_t line_index, std::size_t column) {
    if (viewport_ == nullptr) {
      return false;
    }
    if (tokens_ == nullptr || line_index != line_index_) {
      tokens_ = &viewport_->HighlightedLineTokens(line_index);
      line_index_ = line_index;
    }
    if (column >= tokens_->size()) {
      return false;
    }
    const SyntaxTokenKind kind = (*tokens_)[column];
    return kind == SyntaxTokenKind::String || kind == SyntaxTokenKind::Comment;
  }

 private:
  const TextViewport* viewport_ = nullptr;
  const std::vector<SyntaxTokenKind>* tokens_ = nullptr;
  std::size_t line_index_ = 0;
};

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

bool MatchForwardFromOpener(const WindowLines& lines,
                            std::size_t open_line,
                            std::size_t open_col,
                            std::size_t max_lines_each_side,
                            LineSuppressionCursor& suppression,
                            BracketMatchPair* out) {
  const char open_ch = lines[open_line][open_col];
  const char close_ch = Opposite(open_ch);
  int depth = 1;
  std::size_t end_line = open_line + max_lines_each_side;
  if (end_line > lines.size()) end_line = lines.size();

  std::size_t line = open_line;
  std::size_t col = open_col + 1;
  std::size_t budget = kMaxBracketMatchScanBytes;
  while (line < end_line) {
    std::string_view text = lines[line];
    while (col < text.size()) {
      if (budget-- == 0) {
        return false;
      }
      if (suppression.SuppressedAt(line, col)) {
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

bool MatchBackwardFromCloser(const WindowLines& lines,
                             std::size_t close_line,
                             std::size_t close_col,
                             std::size_t max_lines_each_side,
                             LineSuppressionCursor& suppression,
                             BracketMatchPair* out) {
  const char close_ch = lines[close_line][close_col];
  const char open_ch = Opposite(close_ch);
  int depth = 1;

  std::size_t bottom = close_line >= max_lines_each_side ? close_line - max_lines_each_side : 0;
  std::size_t budget = kMaxBracketMatchScanBytes;

  if (close_col > 0) {
    std::string_view text = lines[close_line];
    std::size_t col = close_col;
    while (col > 0) {
      --col;
      if (budget-- == 0) {
        return false;
      }
      if (suppression.SuppressedAt(close_line, col)) {
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

  std::size_t line = close_line;
  while (line > bottom) {
    --line;
    std::string_view text = lines[line];
    std::size_t col = text.size();
    while (col > 0) {
      --col;
      if (budget-- == 0) {
        return false;
      }
      if (suppression.SuppressedAt(line, col)) {
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

std::optional<BracketMatchPair> FindBracketMatchInWindow(
    const WindowLines& lines,
    std::size_t caret_line,
    std::size_t caret_column,
    std::size_t max_lines_each_side,
    const TextViewport* syntax_viewport) {
  if (caret_line >= lines.size()) return std::nullopt;
  std::string_view current = lines[caret_line];
  LineSuppressionCursor suppression(syntax_viewport);

  auto try_at = [&](std::size_t col) -> std::optional<BracketMatchPair> {
    if (col >= current.size()) return std::nullopt;
    if (suppression.SuppressedAt(caret_line, col)) {
      return std::nullopt;
    }
    char c = current[col];
    BracketMatchPair pair;
    if (IsOpener(c)) {
      if (MatchForwardFromOpener(lines, caret_line, col, max_lines_each_side, suppression,
                                  &pair)) {
        pair.caret_at_opener = true;
        return pair;
      }
    } else if (IsCloser(c)) {
      if (MatchBackwardFromCloser(lines, caret_line, col, max_lines_each_side, suppression,
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

}

std::optional<BracketMatchPair> FindBracketMatchInLines(
    const std::vector<std::string_view>& lines,
    std::size_t caret_line,
    std::size_t caret_column,
    std::size_t max_lines_each_side,
    const TextViewport* syntax_viewport) {
  return FindBracketMatchInWindow(WindowLines{&lines, 0}, caret_line, caret_column,
                                  max_lines_each_side, syntax_viewport);
}

std::optional<BracketMatchPair> FindBracketMatch(const TextViewport& viewport,
                                                 std::size_t caret_line,
                                                 std::size_t caret_column,
                                                 std::size_t max_lines_each_side) {
  const auto& lines = viewport.lines();
  const std::size_t line_count = lines.size();
  if (caret_line >= line_count) {
    return std::nullopt;
  }
  if (lines.LineLength(caret_line) > runtime_syntax::kMaxHighlightLineBytes) {
    util::AddPerformanceCounter(util::PerfCounterId::EditorBracketMatchLineTooLong);
    return std::nullopt;
  }
  const std::size_t lo = caret_line > max_lines_each_side ? caret_line - max_lines_each_side : 0;
  const std::size_t hi = std::min(line_count, caret_line + max_lines_each_side + 1);
  thread_local std::vector<std::string_view> window;
  window.clear();
  window.reserve(hi - lo);
  for (std::size_t i = lo; i < hi; ++i) {
    window.push_back(lines.LineView(i));
  }
  return FindBracketMatchInWindow(WindowLines{&window, lo}, caret_line, caret_column,
                                  max_lines_each_side, &viewport);
}

}
