#include "editor/IndentDetect.h"

#include <array>

namespace microide::editor {

namespace {

bool LineLooksBlank(const std::string& line) {
  for (char c : line) {
    if (c != ' ' && c != '\t') return false;
  }
  return true;
}

std::size_t LeadingSpaces(const std::string& line) {
  std::size_t i = 0;
  while (i < line.size() && line[i] == ' ') ++i;
  return i;
}

bool LineLeadsWithTab(const std::string& line) {
  return !line.empty() && line[0] == '\t';
}

}  // namespace

IndentDetection DetectIndent(const std::vector<std::string>& lines,
                             std::size_t max_inspect_lines) {
  IndentDetection out;
  std::size_t inspected = 0;
  std::size_t tab_count = 0;
  std::size_t space_count = 0;
  std::array<std::size_t, 9> width_hits{};  // index 1..8

  std::size_t prev_indent = 0;
  for (const std::string& line : lines) {
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
