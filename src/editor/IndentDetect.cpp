#include "editor/IndentDetect.h"

#include <array>
#include <cstdlib>
#include <string_view>

namespace microide::editor {

namespace {

constexpr std::size_t kMaxStep = 8;

struct LeadingWhitespace {
  std::size_t length = 0;  // bytes before the first non-blank byte
  std::size_t spaces = 0;
  std::size_t tabs = 0;
  bool has_content = false;
};

LeadingWhitespace ScanLeading(std::string_view line) {
  LeadingWhitespace out;
  for (char c : line) {
    if (c == ' ') {
      ++out.spaces;
    } else if (c == '\t') {
      ++out.tabs;
    } else {
      out.has_content = true;
      break;
    }
    ++out.length;
  }
  return out;
}

struct SpacesDiff {
  std::size_t spaces_diff = 0;
  bool looks_like_alignment = false;
};

// VS Code's spacesDiff: the change in indentation between two content lines,
// measured past their common leading prefix.
SpacesDiff ComputeSpacesDiff(std::string_view a, std::size_t a_length, std::string_view b,
                             std::size_t b_length) {
  SpacesDiff out;
  std::size_t i = 0;
  while (i < a_length && i < b_length && a[i] == b[i]) {
    ++i;
  }
  std::size_t a_spaces = 0;
  std::size_t a_tabs = 0;
  for (std::size_t j = i; j < a_length; ++j) {
    (a[j] == ' ' ? a_spaces : a_tabs)++;
  }
  std::size_t b_spaces = 0;
  std::size_t b_tabs = 0;
  for (std::size_t j = i; j < b_length; ++j) {
    (b[j] == ' ' ? b_spaces : b_tabs)++;
  }
  if ((a_spaces > 0 && a_tabs > 0) || (b_spaces > 0 && b_tabs > 0)) {
    return out;  // mixed run: no evidence
  }
  const std::size_t tabs_diff = a_tabs > b_tabs ? a_tabs - b_tabs : b_tabs - a_tabs;
  const std::size_t spaces_diff = a_spaces > b_spaces ? a_spaces - b_spaces : b_spaces - a_spaces;
  if (tabs_diff == 0) {
    out.spaces_diff = spaces_diff;
    // `foo(a,` followed by a line whose content starts under `a`: alignment,
    // not an indentation step.
    if (spaces_diff > 0 && b_spaces >= 1 && b_spaces - 1 < a.size() && b_spaces < b.size() &&
        b[b_spaces] != ' ' && a[b_spaces - 1] == ' ' && a.back() == ',') {
      out.looks_like_alignment = true;
    }
    return out;
  }
  if (spaces_diff % tabs_diff == 0) {
    out.spaces_diff = spaces_diff / tabs_diff;
  }
  return out;
}

}  // namespace

IndentDetection DetectIndent(LineSpan lines, std::size_t max_inspect_lines) {
  IndentDetection out;
  std::size_t inspected = 0;
  std::size_t tab_lines = 0;
  std::size_t space_lines = 0;
  std::array<std::size_t, kMaxStep + 1> step_hits{};

  std::string_view previous;
  std::size_t previous_length = 0;
  const std::size_t line_count = lines.size();
  for (std::size_t line_index = 0; line_index < line_count && inspected < max_inspect_lines;
       ++line_index) {
    const std::string_view line = lines[line_index];
    const LeadingWhitespace leading = ScanLeading(line);
    if (!leading.has_content) {
      continue;  // blank lines carry no indentation and do not break a run
    }
    ++inspected;
    if (leading.tabs > 0) {
      ++tab_lines;
    } else if (leading.spaces > 1) {
      ++space_lines;
    }
    const SpacesDiff diff = ComputeSpacesDiff(previous, previous_length, line, leading.length);
    if (diff.looks_like_alignment) {
      continue;  // the aligned line is not the next line's predecessor either
    }
    if (diff.spaces_diff <= kMaxStep) {
      step_hits[diff.spaces_diff]++;
    }
    previous = line;
    previous_length = leading.length;
  }

  out.non_blank_lines_inspected = inspected;
  if (tab_lines != space_lines) {
    out.soft_tabs = tab_lines < space_lines;
  }
  if (tab_lines > space_lines) {
    return out;  // a tab file's step is the caller's tab size
  }

  std::size_t best_hits = 0;
  std::size_t best = 0;
  for (std::size_t candidate : {2, 4, 6, 8, 3, 5, 7}) {
    if (step_hits[candidate] > best_hits) {
      best_hits = step_hits[candidate];
      best = candidate;
    }
  }
  if (best == 4 && step_hits[2] > 0 && step_hits[2] * 2 >= step_hits[4]) {
    best = 2;
  }
  if (best != 0) {
    out.indent_width = best;
  }
  return out;
}

}  // namespace microide::editor
