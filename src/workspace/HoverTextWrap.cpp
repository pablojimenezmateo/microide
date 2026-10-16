#include "workspace/HoverTextWrap.h"

#include <utility>

namespace microide::workspace {

namespace {

// One line of the text with its whitespace runs collapsed and its ends trimmed.
std::string CollapseSpaces(std::string_view line) {
  std::string out;
  out.reserve(line.size());
  bool last_was_space = true;
  for (const char c : line) {
    const bool space = c == ' ' || c == '\t' || c == '\r';
    if (space) {
      if (!last_was_space) {
        out.push_back(' ');
      }
      last_was_space = true;
      continue;
    }
    out.push_back(c);
    last_was_space = false;
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

std::vector<std::string_view> Words(std::string_view line) {
  std::vector<std::string_view> words;
  std::size_t start = 0;
  while (start < line.size()) {
    const std::size_t end = line.find(' ', start);
    const std::size_t stop = end == std::string_view::npos ? line.size() : end;
    if (stop > start) {
      words.push_back(line.substr(start, stop - start));
    }
    start = stop + 1;
  }
  return words;
}

}  // namespace

std::vector<std::string> WrapHoverText(std::string_view text, float max_width,
                                       std::size_t max_lines, const HoverTextMeasure& measure) {
  std::vector<std::string> lines;
  if (max_lines == 0 || max_width <= 0.0f) {
    return lines;
  }
  // The text's own lines, spaces collapsed, blank runs folded to one blank, no
  // blank at either end.
  std::vector<std::string> paragraphs;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::size_t stop = end == std::string_view::npos ? text.size() : end;
    std::string line = CollapseSpaces(text.substr(start, stop - start));
    if (!line.empty() || (!paragraphs.empty() && !paragraphs.back().empty())) {
      paragraphs.push_back(std::move(line));
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  while (!paragraphs.empty() && paragraphs.back().empty()) {
    paragraphs.pop_back();
  }

  // Wrap each paragraph by words; a single word wider than the card is truncated.
  std::vector<std::string> wrapped;
  for (const std::string& paragraph : paragraphs) {
    if (paragraph.empty()) {
      wrapped.emplace_back();
      continue;
    }
    if (measure.width(paragraph) <= max_width) {
      wrapped.push_back(paragraph);
      continue;
    }
    std::string line;
    std::string candidate;
    for (const std::string_view word : Words(paragraph)) {
      if (line.empty()) {
        line.assign(word);
        continue;
      }
      candidate.assign(line);
      candidate += ' ';
      candidate += word;
      if (measure.width(candidate) > max_width) {
        wrapped.push_back(measure.truncate(line, max_width));
        line.assign(word);
      } else {
        line.swap(candidate);
      }
    }
    if (!line.empty()) {
      wrapped.push_back(measure.truncate(line, max_width));
    }
  }

  if (wrapped.size() <= max_lines) {
    return wrapped;
  }
  // Over the cap: keep the first max_lines - 1, and let the last line carry the
  // rest, run together and truncated.
  lines.assign(std::make_move_iterator(wrapped.begin()),
               std::make_move_iterator(wrapped.begin() + static_cast<std::ptrdiff_t>(max_lines - 1)));
  std::string rest;
  for (std::size_t i = max_lines - 1; i < wrapped.size(); ++i) {
    if (wrapped[i].empty()) {
      continue;
    }
    if (!rest.empty()) {
      rest += ' ';
    }
    rest += wrapped[i];
  }
  lines.push_back(measure.truncate(rest, max_width));
  return lines;
}

}  // namespace microide::workspace
