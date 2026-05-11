#include "editor/FoldingModel.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

#include "editor/TextViewport.h"

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
struct StackEntry {
  char open;
  char close;
  std::size_t line;
};

const std::pair<char, char>* FindPair(char ch,
                                      const std::vector<std::pair<char, char>>& pairs) {
  for (const auto& p : pairs) {
    if (p.first == ch || p.second == ch) return &p;
  }
  return nullptr;
}

bool IsSuppressedBracketToken(const TextViewport* viewport,
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

// Silent bracket walk used to seed the bracket stack at `resume_line`.
bool BuildBracketStackPrefix(const std::vector<std::string>& lines,
                             const std::vector<std::pair<char, char>>& pairs,
                             std::size_t prefix_end_exclusive,
                             std::size_t max_line_visits,
                             std::size_t& lines_visited,
                             std::vector<StackEntry>& stack,
                             bool& complete,
                             const TextViewport* syntax_viewport) {
  if (pairs.empty() || prefix_end_exclusive == 0) {
    stack.clear();
    return true;
  }
  stack.clear();
  stack.reserve(64);
  for (std::size_t line_index = 0; line_index < prefix_end_exclusive &&
                                  line_index < lines.size();
       ++line_index) {
    if (max_line_visits != 0 && lines_visited >= max_line_visits) {
      complete = false;
      return false;
    }
    lines_visited++;
    const std::string& line = lines[line_index];
    for (std::size_t column = 0; column < line.size(); ++column) {
      if (IsSuppressedBracketToken(syntax_viewport, line_index, column)) {
        continue;
      }
      const char c = line[column];
      const auto* p = FindPair(c, pairs);
      if (p == nullptr) continue;
      if (p->first == p->second) continue;
      if (c == p->first) {
        stack.push_back({p->first, p->second, line_index});
      } else if (c == p->second && !stack.empty() && stack.back().close == c) {
        stack.pop_back();
      }
    }
  }
  return true;
}

// Lines `[begin_line, size)` with emission; honours `stack` seed from prefix walk.
void ScanBracketRangesTail(const std::vector<std::string>& lines,
                           const std::vector<std::pair<char, char>>& pairs,
                           std::size_t begin_line,
                           std::size_t end_line_exclusive,
                           std::vector<StackEntry> stack,
                           std::size_t max_line_visits,
                           std::size_t& lines_visited,
                           std::vector<FoldRange>& out_ranges,
                           bool& complete,
                           const TextViewport* syntax_viewport) {
  if (pairs.empty()) return;
  const std::size_t scan_end = std::min(end_line_exclusive, lines.size());
  for (std::size_t line_index = begin_line; line_index < scan_end; ++line_index) {
    if (max_line_visits != 0 && lines_visited >= max_line_visits) {
      complete = false;
      return;
    }
    lines_visited++;
    const std::string& line = lines[line_index];
    for (std::size_t column = 0; column < line.size(); ++column) {
      if (IsSuppressedBracketToken(syntax_viewport, line_index, column)) {
        continue;
      }
      const char c = line[column];
      const auto* p = FindPair(c, pairs);
      if (p == nullptr) continue;
      if (p->first == p->second) continue;
      if (c == p->first) {
        stack.push_back({p->first, p->second, line_index});
      } else if (c == p->second && !stack.empty() && stack.back().close == c) {
        const StackEntry top = stack.back();
        stack.pop_back();
        if (line_index > top.line) {
          out_ranges.push_back(FoldRange{top.line, line_index, FoldSource::Bracket});
        }
      }
    }
  }
}

void ScanBracketRanges(const std::vector<std::string>& lines,
                       const std::vector<std::pair<char, char>>& pairs,
                       std::size_t end_line_exclusive,
                       std::size_t max_line_visits,
                       std::vector<FoldRange>& out_ranges,
                       bool& complete,
                       const TextViewport* syntax_viewport) {
  if (pairs.empty()) {
    return;
  }
  std::size_t lines_visited = 0;
  ScanBracketRangesTail(lines, pairs, 0, end_line_exclusive, /*stack=*/{}, max_line_visits,
                        lines_visited, out_ranges, complete, syntax_viewport);
}
void ScanIndentRanges(const std::vector<std::string>& lines,
                      std::size_t end_line_exclusive,
                      std::size_t tab_size,
                      const std::vector<FoldRange>& bracket_ranges,
                      std::size_t max_lines,
                      std::vector<FoldRange>& out_ranges,
                      bool& complete) {
  const std::size_t scan_end = std::min(end_line_exclusive, lines.size());
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
  for (std::size_t i = 0; i < scan_end; ++i) {
    if (max_lines != 0 && scanned >= max_lines) {
      complete = false;
      break;
    }
    ++scanned;
    indents[i] = MeasureIndent(lines[i], tab_size);
  }

  for (std::size_t i = 0; i < scan_end; ++i) {
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
    while (body_start < scan_end && indents[body_start] == kSentinelIndent) {
      ++body_start;
    }
    if (body_start >= scan_end) break;
    if (indents[body_start] <= opener_indent) continue;
    // Walk forward until indent <= opener_indent or EOF.
    std::size_t closer = body_start;
    while (closer + 1 < scan_end) {
      const std::size_t next_indent = indents[closer + 1];
      if (next_indent != kSentinelIndent && next_indent <= opener_indent) break;
      ++closer;
    }
    if (closer > i) {
      out_ranges.push_back(FoldRange{i, closer, FoldSource::Indent});
    }
  }
}

void RemapCollapsedFlags(const std::vector<FoldRange>& previous_ranges,
                         const std::vector<bool>& previous_collapsed,
                         const std::vector<FoldRange>& new_ranges,
                         std::vector<bool>& out_collapsed) {
  out_collapsed.assign(new_ranges.size(), false);
  for (std::size_t i = 0; i < new_ranges.size(); ++i) {
    for (std::size_t j = 0; j < previous_ranges.size(); ++j) {
      if (previous_ranges[j].opener_line == new_ranges[i].opener_line &&
          previous_ranges[j].closer_line == new_ranges[i].closer_line &&
          previous_ranges[j].source == new_ranges[i].source) {
        out_collapsed[i] = previous_collapsed[j];
        break;
      }
    }
  }
}

void SortDedupeRangesByOpener(std::vector<FoldRange>& ranges) {
  std::sort(ranges.begin(), ranges.end(), [](const FoldRange& a, const FoldRange& b) {
    if (a.opener_line != b.opener_line) {
      return a.opener_line < b.opener_line;
    }
    if (a.source != b.source) {
      return static_cast<int>(a.source) < static_cast<int>(b.source);
    }
    return a.closer_line > b.closer_line;
  });
  ranges.erase(std::unique(ranges.begin(), ranges.end(),
                           [](const FoldRange& a, const FoldRange& b) {
                             return a.opener_line == b.opener_line;
                           }),
               ranges.end());
}

}  // namespace

bool FoldingModel::Compute(const std::vector<std::string>& lines,
                           const ComputeOptions& options) {
  return ComputeWithBudget(lines, options, /*max_lines=*/0,
                           std::numeric_limits<std::size_t>::max(),
                           std::numeric_limits<std::size_t>::max(),
                           nullptr);
}

bool FoldingModel::ComputeWithBudget(const std::vector<std::string>& lines,
                                     const ComputeOptions& options,
                                     std::size_t max_lines,
                                     std::size_t incremental_resume_line,
                                     std::size_t target_end_exclusive,
                                     const TextViewport* syntax_viewport) {
  const std::vector<FoldRange> previous_ranges = ranges_;
  const std::vector<bool> previous_collapsed = collapsed_;
  const std::size_t line_count = lines.size();
  constexpr std::size_t kNoResume = std::numeric_limits<std::size_t>::max();
  const std::size_t scan_end =
      std::min(target_end_exclusive == kNoResume ? line_count : target_end_exclusive, line_count);

  ranges_.clear();
  collapsed_.clear();
  complete_ = scan_end >= line_count;

  auto merge_indent_and_finish = [&](std::vector<FoldRange> bracket_ranges,
                                     bool bracket_scan_complete) -> bool {
    std::vector<FoldRange> indent_ranges;
    if (options.use_indent_source) {
      ScanIndentRanges(lines, scan_end, options.tab_size, bracket_ranges, max_lines,
                       indent_ranges, complete_);
    }

    ranges_.reserve(bracket_ranges.size() + indent_ranges.size());
    ranges_.insert(ranges_.end(), bracket_ranges.begin(), bracket_ranges.end());
    ranges_.insert(ranges_.end(), indent_ranges.begin(), indent_ranges.end());
    SortDedupeRangesByOpener(ranges_);
    RemapCollapsedFlags(previous_ranges, previous_collapsed, ranges_, collapsed_);
    complete_ = complete_ && bracket_scan_complete && scan_end >= line_count;
    resolved_prefix_line_count_ = scan_end;
    ++revision_;
    return complete_;
  };

  const bool resume_valid = incremental_resume_line != kNoResume &&
                            incremental_resume_line > 0 &&
                            incremental_resume_line < line_count &&
                            !options.bracket_pairs.empty();

  bool kept_prefix_exists = false;
  if (resume_valid) {
    for (const FoldRange& r : previous_ranges) {
      if (r.source == FoldSource::Bracket && r.closer_line < incremental_resume_line) {
        kept_prefix_exists = true;
        break;
      }
    }
  }

  std::vector<FoldRange> bracket_ranges;
  if (!resume_valid || !kept_prefix_exists) {
    ScanBracketRanges(lines, options.bracket_pairs, scan_end, max_lines, bracket_ranges, complete_,
                      syntax_viewport);
    return merge_indent_and_finish(std::move(bracket_ranges), complete_);
  }

  bool prefix_lines_complete = true;
  std::size_t lines_visited = 0;
  std::vector<StackEntry> prefix_stack;
  if (!BuildBracketStackPrefix(lines, options.bracket_pairs, incremental_resume_line, max_lines,
                               lines_visited, prefix_stack, prefix_lines_complete,
                               syntax_viewport)) {
    bracket_ranges.clear();
    complete_ = true;
    ScanBracketRanges(lines, options.bracket_pairs, scan_end, max_lines, bracket_ranges, complete_,
                      syntax_viewport);
    return merge_indent_and_finish(std::move(bracket_ranges), complete_);
  }

  std::vector<FoldRange> kept_brackets;
  kept_brackets.reserve(previous_ranges.size());
  for (const FoldRange& r : previous_ranges) {
    if (r.source == FoldSource::Bracket && r.closer_line < incremental_resume_line) {
      kept_brackets.push_back(r);
    }
  }

  std::vector<FoldRange> tail_brackets;
  bool tail_complete = prefix_lines_complete;
  ScanBracketRangesTail(lines, options.bracket_pairs, incremental_resume_line, scan_end,
                        std::move(prefix_stack), max_lines, lines_visited,
                        tail_brackets, tail_complete, syntax_viewport);

  bracket_ranges.reserve(kept_brackets.size() + tail_brackets.size());
  bracket_ranges.insert(bracket_ranges.end(), kept_brackets.begin(), kept_brackets.end());
  bracket_ranges.insert(bracket_ranges.end(), tail_brackets.begin(), tail_brackets.end());
  return merge_indent_and_finish(std::move(bracket_ranges), tail_complete);
}

bool FoldingModel::EnsureFoldsForVisibleRange(
    const std::vector<std::string>& lines,
    const ComputeOptions& options,
    std::size_t visible_start_line,
    std::size_t visible_end_line,
    std::size_t max_lines,
    std::size_t incremental_resume_line,
    const TextViewport* syntax_viewport) {
  constexpr std::size_t kVisibleLookAhead = 32;
  const std::size_t line_count = lines.size();
  if (line_count == 0) {
    Clear();
    dirty_ = false;
    resolved_prefix_line_count_ = 0;
    return true;
  }

  const std::size_t target_end =
      std::min(line_count, std::max(visible_start_line, visible_end_line) + kVisibleLookAhead + 1);
  if (!dirty_ && resolved_prefix_line_count_ >= target_end) {
    return true;
  }

  const std::size_t work_budget = max_lines == 0 ? target_end : std::max(max_lines, target_end);
  ComputeWithBudget(lines, options, work_budget, incremental_resume_line, target_end,
                    syntax_viewport);
  dirty_ = false;
  resolved_prefix_line_count_ = target_end;
  return true;
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

std::optional<FoldRange> FoldingModel::InnermostFoldContaining(std::size_t line) const {
  std::optional<FoldRange> best;
  for (const auto& r : ranges_) {
    if (r.opener_line <= line && line <= r.closer_line) {
      if (!best.has_value() || r.opener_line > best->opener_line) {
        best = r;
      }
    }
  }
  return best;
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
  resolved_prefix_line_count_ = 0;
  ++revision_;
}

}  // namespace microide::editor
