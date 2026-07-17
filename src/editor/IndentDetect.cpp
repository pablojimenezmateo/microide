#include "editor/IndentDetect.h"

#include <array>

namespace microide::editor {

namespace {

bool LineLooksBlank(std::string_view line) {
  for (char c : line) {
    if (c != ' ' && c != '\t') return false;
  }
  return true;
}

std::size_t LeadingSpaces(std::string_view line) {
  std::size_t i = 0;
  while (i < line.size() && line[i] == ' ') ++i;
  return i;
}

// True when the line's leading whitespace run contains a tab anywhere before the
// first non-whitespace byte. A mixed run like `"  \tcode"` (spaces then a tab) is
// tab-indented in effect; classifying it by its first byte alone would miscount
// the file as space-indented.
bool LineLeadsWithTab(std::string_view line) {
  for (char c : line) {
    if (c == '\t') return true;
    if (c != ' ') return false;
  }
  return false;
}

}  // namespace

IndentDetection DetectIndent(LineSpan lines, std::size_t max_inspect_lines) {
  IndentDetection out;
  std::size_t inspected = 0;
  std::size_t tab_count = 0;
  std::size_t space_count = 0;
  std::array<std::size_t, 9> width_hits{};  // index 1..8

  std::size_t prev_indent = 0;
  const std::size_t line_count = lines.size();
  for (std::size_t line_index = 0; line_index < line_count; ++line_index) {
    const std::string_view line = lines[line_index];
    if (inspected >= max_inspect_lines) break;
    if (LineLooksBlank(line)) continue;
    ++inspected;

    if (LineLeadsWithTab(line)) {
      ++tab_count;
      prev_indent = 0;
      continue;
    }
    std::size_t lead = LeadingSpaces(line);
    if (lead == 0) {
      prev_indent = 0;
      continue;
    }
    ++space_count;
    if (lead > prev_indent) {
      std::size_t step = lead - prev_indent;
      if (step >= 1 && step <= 8) {
        width_hits[step]++;
      }
    }
    prev_indent = lead;
  }

  out.non_blank_lines_inspected = inspected;
  if (tab_count == 0 && space_count == 0) return out;

  if (tab_count > space_count) {
    out.soft_tabs = false;
    out.indent_width = 4;  // tab visual width default; tab_size handles paint
    out.detected = true;
    return out;
  }

  out.soft_tabs = true;
  std::size_t best = 4;
  std::size_t best_hits = 0;
  for (std::size_t w = 2; w <= 8; ++w) {
    if (width_hits[w] > best_hits) {
      best_hits = width_hits[w];
      best = w;
    }
  }
  if (best_hits == 0) {
    // No positive step seen; keep default 4-space.
    return out;
  }
  out.indent_width = best;
  out.detected = true;
  return out;
}

}  // namespace microide::editor
