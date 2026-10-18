#include "editor/SmartSelect.h"

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

#include "editor/BracketScanner.h"

namespace microide::editor {
namespace {

bool Before(const TextPosition& a, const TextPosition& b) {
  return a.line < b.line || (a.line == b.line && a.column < b.column);
}

bool Contains(const SelectionRange& outer, const SelectionRange& inner) {
  return !Before(inner.start, outer.start) && !Before(outer.end, inner.end);
}

bool SameRange(const SelectionRange& a, const SelectionRange& b) {
  return a.start == b.start && a.end == b.end;
}

// The string literals of one line as [open quote, close quote] byte columns. A
// backslash escapes the next byte; an unterminated quote runs to the line's end
// and is not a span.
std::vector<std::array<std::size_t, 2>> StringSpans(std::string_view line) {
  std::vector<std::array<std::size_t, 2>> spans;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char quote = line[i];
    if (quote != '"' && quote != '\'' && quote != '`') {
      continue;
    }
    std::size_t j = i + 1;
    while (j < line.size() && line[j] != quote) {
      j += line[j] == '\\' ? 2 : 1;
    }
    if (j >= line.size()) {
      break;
    }
    spans.push_back({i, j});
    i = j;
  }
  return spans;
}

bool InsideString(const std::vector<std::array<std::size_t, 2>>& spans, std::size_t column) {
  for (const auto& span : spans) {
    if (column > span[0] && column < span[1]) {
      return true;
    }
  }
  return false;
}

int OpenerIndex(char c) {
  for (std::size_t i = 0; i < std::size(kDefaultBracketSet); ++i) {
    if (kDefaultBracketSet[i].open == c) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int CloserIndex(char c) {
  for (std::size_t i = 0; i < std::size(kDefaultBracketSet); ++i) {
    if (kDefaultBracketSet[i].close == c) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// Walks bytes outside string literals from `from` (inclusive) in `direction`,
// calling `visit(position, c)` until it returns true or the scan cap is reached.
template <typename Visit>
std::optional<TextPosition> Walk(const TextBuffer& lines, TextPosition from, int direction,
                                 Visit&& visit) {
  std::size_t budget = kMaxBracketMatchScanBytes;
  std::size_t line_index = from.line;
  std::optional<std::size_t> column = from.column;
  while (line_index < lines.size()) {
    const std::string_view line = lines.LineView(line_index);
    const auto spans = StringSpans(line);
    if (direction > 0) {
      for (std::size_t c = column.value_or(0); c < line.size(); ++c) {
        if (budget-- == 0) {
          return std::nullopt;
        }
        if (!InsideString(spans, c) && visit(TextPosition{line_index, c}, line[c])) {
          return TextPosition{line_index, c};
        }
      }
      ++line_index;
    } else {
      std::size_t c = column.has_value() ? std::min(*column, line.size()) : line.size();
      while (c-- > 0) {
        if (budget-- == 0) {
          return std::nullopt;
        }
        if (!InsideString(spans, c) && visit(TextPosition{line_index, c}, line[c])) {
          return TextPosition{line_index, c};
        }
      }
      if (line_index == 0) {
        return std::nullopt;
      }
      --line_index;
    }
    column.reset();
  }
  return std::nullopt;
}

// The innermost bracket pair around [start, end): its opener's and closer's
// positions. The opener is the first unmatched one walking back from `start`; the
// closer, the one matching it walking forward from `end`.
std::optional<std::array<TextPosition, 2>> EnclosingPair(const TextBuffer& lines, TextPosition start,
                                                         TextPosition end) {
  std::vector<int> pending_closers;
  int kind = -1;
  const std::optional<TextPosition> opener =
      Walk(lines, start, -1, [&](TextPosition, char c) {
        if (const int closer = CloserIndex(c); closer >= 0) {
          pending_closers.push_back(closer);
          return false;
        }
        const int open = OpenerIndex(c);
        if (open < 0) {
          return false;
        }
        if (pending_closers.empty()) {
          kind = open;
          return true;
        }
        pending_closers.pop_back();
        return false;
      });
  if (!opener.has_value()) {
    return std::nullopt;
  }
  // From `end`, the closer of `kind` with everything opened since closed again.
  std::vector<int> pending_openers;
  const std::optional<TextPosition> closer =
      Walk(lines, end, 1, [&](TextPosition, char c) {
        if (const int open = OpenerIndex(c); open >= 0) {
          pending_openers.push_back(open);
          return false;
        }
        const int close = CloserIndex(c);
        if (close < 0) {
          return false;
        }
        if (pending_openers.empty()) {
          return close == kind;
        }
        pending_openers.pop_back();
        return false;
      });
  if (!closer.has_value()) {
    return std::nullopt;
  }
  return std::array<TextPosition, 2>{*opener, *closer};
}

}  // namespace

std::optional<SelectionRange> ExpandSelectionRange(const TextBuffer& lines, SelectionRange current) {
  if (lines.size() == 0) {
    return std::nullopt;
  }
  std::optional<SelectionRange> best;
  const auto offer = [&](SelectionRange candidate) {
    if (!Contains(candidate, current) || SameRange(candidate, current)) {
      return;
    }
    if (!best.has_value() || Contains(*best, candidate)) {
      best = candidate;
    }
  };

  const std::size_t last_line = lines.size() - 1;
  if (current.start.line == current.end.line) {
    const std::size_t row = current.start.line;
    const std::string_view line = lines.LineView(row);
    // The identifier run touching the selection -- also a call's name joining its
    // argument list once the list is selected, as VS Code's ladder does.
    std::size_t word_start = std::min(current.start.column, line.size());
    std::size_t word_end = std::min(current.end.column, line.size());
    while (word_start > 0 && IsIdentifierByte(line[word_start - 1])) {
      --word_start;
    }
    while (word_end < line.size() && IsIdentifierByte(line[word_end])) {
      ++word_end;
    }
    if (word_end > word_start) {
      offer({{row, word_start}, {row, word_end}});
    }
    for (const auto& span : StringSpans(line)) {
      offer({{row, span[0] + 1}, {row, span[1]}});
      offer({{row, span[0]}, {row, span[1] + 1}});
    }
    std::size_t first = 0;
    while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) {
      ++first;
    }
    std::size_t last = line.size();
    while (last > first && (line[last - 1] == ' ' || line[last - 1] == '\t')) {
      --last;
    }
    offer({{row, first}, {row, last}});
    offer(row < last_line ? SelectionRange{{row, 0}, {row + 1, 0}}
                          : SelectionRange{{row, 0}, {row, line.size()}});
  }

  // Enclosing bracket pairs, innermost first; stop at the first that offers
  // something (anything further out is larger than it).
  TextPosition from = current.start;
  TextPosition to = current.end;
  for (int depth = 0; depth < 64; ++depth) {
    const auto pair = EnclosingPair(lines, from, to);
    if (!pair.has_value()) {
      break;
    }
    const SelectionRange content{{(*pair)[0].line, (*pair)[0].column + 1}, (*pair)[1]};
    const SelectionRange whole{(*pair)[0], {(*pair)[1].line, (*pair)[1].column + 1}};
    offer(content);
    offer(whole);
    if (best.has_value() && Contains(whole, *best)) {
      break;  // every pair further out is larger still
    }
    from = (*pair)[0];
    to = {(*pair)[1].line, (*pair)[1].column + 1};
  }

  offer({{0, 0}, {last_line, lines.LineView(last_line).size()}});
  return best;
}

}  // namespace microide::editor
