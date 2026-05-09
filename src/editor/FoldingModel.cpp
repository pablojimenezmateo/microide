#include "editor/FoldingModel.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace microide::editor {

namespace {

constexpr std::size_t kSentinelIndent = static_cast<std::size_t>(-1);

std::size_t MeasureIndent(std::string_view line, std::size_t tab_size) {
  if (tab_size == 0) tab_size = 1;
  std::size_t indent = 0;
  for (char c : line) {
    if (c == ' ') {
      ++indent;
    } else if (c == '\t') {
      indent += tab_size - (indent % tab_size);
    } else {
      return indent;
    }
  }
  return kSentinelIndent;  // line is whitespace-only / blank
}

bool LineIsBlankOrIndentOnly(std::string_view line) {
  for (char c : line) {
    if (c != ' ' && c != '\t') return false;
  }
  return true;
}

// Single-line bracket scanner that emits balanced bracket fold ranges across
// multiple lines. The scanner walks character-by-character; for each open
// bracket it records the line of the opener; the corresponding close on a
// later line emits a fold range. Brackets balanced on the same line are
// ignored (no fold opportunity).
void ScanBracketRanges(const std::vector<std::string>& lines,
                       const std::vector<std::pair<char, char>>& pairs,
                       std::size_t max_lines,
                       std::vector<FoldRange>& out_ranges,
                       bool& complete) {
  if (pairs.empty()) {
    return;
  }
  struct StackEntry {
    char open;
    char close;
    std::size_t line;
  };
  std::vector<StackEntry> stack;
  stack.reserve(64);

  auto find_pair = [&](char ch) -> const std::pair<char, char>* {
    for (const auto& p : pairs) {
      if (p.first == ch || p.second == ch) return &p;
    }
    return nullptr;
  };

  std::size_t scanned = 0;
  for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
    if (max_lines != 0 && scanned >= max_lines) {
      complete = false;
      return;
    }
    ++scanned;
    const std::string& line = lines[line_index];
    for (char c : line) {
      const auto* p = find_pair(c);
      if (p == nullptr) continue;
      if (p->first == p->second) {
        // Symmetric pair (unsupported by fold scan – e.g. quotes).
        continue;
      }
      if (c == p->first) {
        stack.push_back({p->first, p->second, line_index});
      } else if (c == p->second && !stack.empty() && stack.back().close == c) {
        const StackEntry top = stack.back();
        stack.pop_back();
        if (line_index > top.line) {
          out_ranges.push_back(
              FoldRange{top.line, line_index, FoldSource::Bracket});
        }
      }
    }
  }
}

void ScanIndentRanges(const std::vector<std::string>& lines,
                      std::size_t tab_size,
                      const std::vector<FoldRange>& bracket_ranges,
                      std::size_t max_lines,
                      std::vector<FoldRange>& out_ranges,
                      bool& complete) {
  // Build a sorted set of openers covered by bracket scans so we don't emit
  // an indent fold on the same opener line.
  std::vector<bool> bracket_opener(lines.size(), false);
  for (const auto& r : bracket_ranges) {
    if (r.opener_line < bracket_opener.size()) {
      bracket_opener[r.opener_line] = true;
    }
  }

  // Precompute indent for every line (sentinel for blank lines) within budget.
  std::vector<std::size_t> indents(lines.size(), kSentinelIndent);
  std::size_t scanned = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (max_lines != 0 && scanned >= max_lines) {
      complete = false;
      break;
    }
    ++scanned;
    indents[i] = MeasureIndent(lines[i], tab_size);
  }

  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (max_lines != 0 && scanned >= max_lines) {
      complete = false;
      break;
    }
    ++scanned;
    if (bracket_opener[i]) continue;
    const std::size_t opener_indent = indents[i];
    if (opener_indent == kSentinelIndent) continue;  // blank line
    if (LineIsBlankOrIndentOnly(lines[i])) continue;
    // Find the next non-blank line to determine if a deeper-indented body starts.
    std::size_t body_start = i + 1;
    while (body_start < lines.size() && indents[body_start] == kSentinelIndent) {
      ++body_start;
    }
    if (body_start >= lines.size()) break;
    if (indents[body_start] <= opener_indent) continue;
    // Walk forward until indent <= opener_indent or EOF.
    std::size_t closer = body_start;
    while (closer + 1 < lines.size()) {
      const std::size_t next_indent = indents[closer + 1];
      if (next_indent != kSentinelIndent && next_indent <= opener_indent) break;
      ++closer;
    }
    if (closer > i) {
      out_ranges.push_back(FoldRange{i, closer, FoldSource::Indent});
    }
  }
}

}  // namespace

bool FoldingModel::Compute(const std::vector<std::string>& lines,
                           const ComputeOptions& options) {
  return ComputeWithBudget(lines, options, /*max_lines=*/0);
}

bool FoldingModel::ComputeWithBudget(const std::vector<std::string>& lines,
                                     const ComputeOptions& options,
                                     std::size_t max_lines) {
  ranges_.clear();
  collapsed_.clear();
  complete_ = true;

  std::vector<FoldRange> bracket_ranges;
  ScanBracketRanges(lines, options.bracket_pairs, max_lines, bracket_ranges,
                    complete_);
  if (!complete_) {
    ranges_ = std::move(bracket_ranges);
    collapsed_.assign(ranges_.size(), false);
    ++revision_;
    return complete_;
  }

  std::vector<FoldRange> indent_ranges;
  if (options.use_indent_source) {
    ScanIndentRanges(lines, options.tab_size, bracket_ranges, max_lines,
                     indent_ranges, complete_);
  }

  ranges_.reserve(bracket_ranges.size() + indent_ranges.size());
  ranges_.insert(ranges_.end(), bracket_ranges.begin(), bracket_ranges.end());
  ranges_.insert(ranges_.end(), indent_ranges.begin(), indent_ranges.end());

  std::sort(ranges_.begin(), ranges_.end(),
            [](const FoldRange& a, const FoldRange& b) {
              if (a.opener_line != b.opener_line) {
                return a.opener_line < b.opener_line;
              }
              if (a.source != b.source) {
                return static_cast<int>(a.source) < static_cast<int>(b.source);
              }
              return a.closer_line > b.closer_line;
            });
  ranges_.erase(std::unique(ranges_.begin(), ranges_.end(),
                            [](const FoldRange& a, const FoldRange& b) {
                              return a.opener_line == b.opener_line;
                            }),
                ranges_.end());
  collapsed_.assign(ranges_.size(), false);
  ++revision_;
  return complete_;
}

namespace {

std::ptrdiff_t IndexOfOpener(const std::vector<FoldRange>& ranges,
                             std::size_t opener_line) {
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    if (ranges[i].opener_line == opener_line) {
      return static_cast<std::ptrdiff_t>(i);
    }
  }
  return -1;
}

}  // namespace

bool FoldingModel::ToggleFold(std::size_t opener_line) {
  const auto idx = IndexOfOpener(ranges_, opener_line);
  if (idx < 0) return false;
  collapsed_[static_cast<std::size_t>(idx)] =
      !collapsed_[static_cast<std::size_t>(idx)];
  ++revision_;
  return true;
}

bool FoldingModel::Collapse(std::size_t opener_line) {
  const auto idx = IndexOfOpener(ranges_, opener_line);
  if (idx < 0) return false;
  if (collapsed_[static_cast<std::size_t>(idx)]) return false;
  collapsed_[static_cast<std::size_t>(idx)] = true;
  ++revision_;
  return true;
}

bool FoldingModel::Expand(std::size_t opener_line) {
  const auto idx = IndexOfOpener(ranges_, opener_line);
  if (idx < 0) return false;
  if (!collapsed_[static_cast<std::size_t>(idx)]) return false;
  collapsed_[static_cast<std::size_t>(idx)] = false;
  ++revision_;
  return true;
}

void FoldingModel::CollapseAll() {
  if (std::all_of(collapsed_.begin(), collapsed_.end(), [](bool collapsed) { return collapsed; })) {
    return;
  }
  std::fill(collapsed_.begin(), collapsed_.end(), true);
  ++revision_;
}

void FoldingModel::ExpandAll() {
  if (std::none_of(collapsed_.begin(), collapsed_.end(), [](bool collapsed) { return collapsed; })) {
    return;
  }
  std::fill(collapsed_.begin(), collapsed_.end(), false);
  ++revision_;
}

bool FoldingModel::IsLineHidden(std::size_t line) const {
  for (std::size_t i = 0; i < ranges_.size(); ++i) {
    if (!collapsed_[i]) continue;
    // A collapsed fold consumes a single visible row anchored on the opener.
    if (line > ranges_[i].opener_line && line <= ranges_[i].closer_line) {
      return true;
    }
  }
  return false;
}

std::optional<FoldRange> FoldingModel::FoldStartingAt(std::size_t line) const {
  for (const auto& r : ranges_) {
    if (r.opener_line == line) return r;
  }
  return std::nullopt;
}

bool FoldingModel::IsCollapsedAtOpener(std::size_t line) const {
  for (std::size_t i = 0; i < ranges_.size(); ++i) {
    if (ranges_[i].opener_line == line) return collapsed_[i];
  }
  return false;
}

void FoldingModel::Clear() {
  ranges_.clear();
  collapsed_.clear();
  complete_ = true;
  dirty_ = true;
  ++revision_;
}

}  // namespace microide::editor
