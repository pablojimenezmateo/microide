#include "editor/FoldingModel.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

// 256-entry table: BracketKind[byte] selects which pair the byte belongs to (1..127), or 0 when the
// byte is not a bracket character at all. The bracket-scan inner loop runs once per source byte, so
// the previous std::vector<std::pair<char,char>> linear scan was paid for every byte. The lookup
// makes the per-byte check a single load + compare.
struct BracketLookupTable {
  // For each input byte: 0 = not a bracket, otherwise index+1 into the source `pairs` vector.
  std::array<std::uint8_t, 256> kind_for_byte{};
  // Cached `(open, close)` for each used kind. Index 0 is unused.
  std::array<std::pair<char, char>, 32> pair_for_kind{};
  bool any_bracket = false;
};

BracketLookupTable BuildBracketLookupTable(const std::vector<std::pair<char, char>>& pairs) {
  BracketLookupTable table;
  // Cap at 31 distinct pairs; FoldingModel inputs ship 3-4 pairs in practice.
  const std::size_t capped = std::min<std::size_t>(pairs.size(), 31);
  for (std::size_t i = 0; i < capped; ++i) {
    const std::uint8_t kind = static_cast<std::uint8_t>(i + 1);
    const auto& pair = pairs[i];
    const auto open_byte = static_cast<unsigned char>(pair.first);
    const auto close_byte = static_cast<unsigned char>(pair.second);
    if (table.kind_for_byte[open_byte] == 0) {
      table.kind_for_byte[open_byte] = kind;
    }
    if (table.kind_for_byte[close_byte] == 0) {
      table.kind_for_byte[close_byte] = kind;
    }
    table.pair_for_kind[kind] = pair;
    table.any_bracket = true;
  }
  return table;
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
  const BracketLookupTable table = BuildBracketLookupTable(pairs);
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
      const auto byte = static_cast<unsigned char>(line[column]);
      const std::uint8_t kind = table.kind_for_byte[byte];
      if (kind == 0) continue;
      if (IsSuppressedBracketToken(syntax_viewport, line_index, column)) {
        continue;
      }
      const auto& pair = table.pair_for_kind[kind];
      if (pair.first == pair.second) continue;
      const char c = static_cast<char>(byte);
      if (c == pair.first) {
        stack.push_back({pair.first, pair.second, line_index});
      } else if (c == pair.second && !stack.empty() && stack.back().close == c) {
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
  const BracketLookupTable table = BuildBracketLookupTable(pairs);
  const std::size_t scan_end = std::min(end_line_exclusive, lines.size());
  for (std::size_t line_index = begin_line; line_index < scan_end; ++line_index) {
    if (max_line_visits != 0 && lines_visited >= max_line_visits) {
      complete = false;
      return;
    }
    lines_visited++;
    const std::string& line = lines[line_index];
    for (std::size_t column = 0; column < line.size(); ++column) {
      const auto byte = static_cast<unsigned char>(line[column]);
      const std::uint8_t kind = table.kind_for_byte[byte];
      if (kind == 0) continue;
      if (IsSuppressedBracketToken(syntax_viewport, line_index, column)) {
        continue;
      }
      const auto& pair = table.pair_for_kind[kind];
      if (pair.first == pair.second) continue;
      const char c = static_cast<char>(byte);
      if (c == pair.first) {
        stack.push_back({pair.first, pair.second, line_index});
      } else if (c == pair.second && !stack.empty() && stack.back().close == c) {
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

// Indexes only the collapsed previous openers. `(opener_line, closer_line, source)` matches the
// equality predicate the previous O(N·M) loop checked.
struct CollapsedOpenerKey {
  std::size_t opener_line;
  std::size_t closer_line;
  FoldSource source;
  bool operator==(const CollapsedOpenerKey& other) const noexcept {
    return opener_line == other.opener_line && closer_line == other.closer_line &&
           source == other.source;
  }
};

void RemapCollapsedFlags(const std::vector<FoldRange>& previous_ranges,
                         const std::vector<bool>& previous_collapsed,
                         std::size_t previous_collapsed_count,
                         const std::vector<FoldRange>& new_ranges,
                         std::vector<bool>& out_collapsed,
                         std::size_t& out_collapsed_count) {
  out_collapsed.assign(new_ranges.size(), false);
  out_collapsed_count = 0;
  if (previous_collapsed_count == 0 || previous_ranges.empty() || new_ranges.empty()) {
    return;
  }

  // Collect just the collapsed previous openers (typically tiny: only ranges
  // the user explicitly toggled), sorted by opener_line for a binary search.
  std::vector<std::pair<std::size_t, std::size_t>> collapsed_index;
  collapsed_index.reserve(previous_collapsed_count);
  for (std::size_t j = 0; j < previous_ranges.size(); ++j) {
    if (previous_collapsed[j]) {
      collapsed_index.emplace_back(previous_ranges[j].opener_line, j);
    }
  }
  std::sort(collapsed_index.begin(), collapsed_index.end());

  for (std::size_t i = 0; i < new_ranges.size(); ++i) {
    const auto& nr = new_ranges[i];
    auto it = std::lower_bound(
        collapsed_index.begin(), collapsed_index.end(),
        std::pair<std::size_t, std::size_t>{nr.opener_line, 0},
        [](const auto& a, const auto& b) { return a.first < b.first; });
    while (it != collapsed_index.end() && it->first == nr.opener_line) {
      const FoldRange& pr = previous_ranges[it->second];
      if (pr.closer_line == nr.closer_line && pr.source == nr.source) {
        out_collapsed[i] = true;
        ++out_collapsed_count;
        break;
      }
      ++it;
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
  // Skip the expensive previous-state copies when nothing was collapsed; the
  // remap is then a no-op and the new state is just `all-false`.
  const std::size_t previous_collapsed_count = collapsed_count_;
  std::vector<FoldRange> previous_ranges;
  std::vector<bool> previous_collapsed;
  if (previous_collapsed_count > 0) {
    previous_ranges = ranges_;
    previous_collapsed = collapsed_;
  }
  const std::size_t line_count = lines.size();
  constexpr std::size_t kNoResume = std::numeric_limits<std::size_t>::max();
  const std::size_t scan_end =
      std::min(target_end_exclusive == kNoResume ? line_count : target_end_exclusive, line_count);

  ranges_.clear();
  collapsed_.clear();
  collapsed_count_ = 0;
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
    if (previous_collapsed_count > 0) {
      RemapCollapsedFlags(previous_ranges, previous_collapsed, previous_collapsed_count,
                          ranges_, collapsed_, collapsed_count_);
    } else {
      collapsed_.assign(ranges_.size(), false);
      collapsed_count_ = 0;
    }
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

// ranges_ is kept sorted by `opener_line` (and de-duplicated to one entry per
// opener) after `SortDedupeRangesByOpener`. Resolve an opener via binary search.
std::ptrdiff_t IndexOfOpener(const std::vector<FoldRange>& ranges,
                             std::size_t opener_line) {
  const auto it = std::lower_bound(
      ranges.begin(), ranges.end(), opener_line,
      [](const FoldRange& r, std::size_t v) { return r.opener_line < v; });
  if (it == ranges.end() || it->opener_line != opener_line) {
    return -1;
  }
  return static_cast<std::ptrdiff_t>(it - ranges.begin());
}

}  // namespace

void FoldingModel::EnsureLookupCache() const {
  if (cached_revision_ == revision_) {
    return;
  }
  cached_collapsed_intervals_.clear();
  cached_collapsed_hi_prefix_max_.clear();
  cached_range_closer_prefix_max_.clear();
  cached_collapsed_intervals_.reserve(ranges_.size());
  for (std::size_t i = 0; i < ranges_.size(); ++i) {
    if (i < collapsed_.size() && collapsed_[i] &&
        ranges_[i].closer_line > ranges_[i].opener_line) {
      cached_collapsed_intervals_.push_back(
          CollapsedInterval{ranges_[i].opener_line + 1, ranges_[i].closer_line});
    }
  }
  // Collapsed intervals are emitted in opener order (ranges_ is sorted by opener),
  // but interleave with non-collapsed entries we skipped, so the result is still
  // sorted by `lo`. Build the prefix running-max of `hi` for IsLineHidden.
  cached_collapsed_hi_prefix_max_.resize(cached_collapsed_intervals_.size());
  {
    std::size_t running = 0;
    for (std::size_t i = 0; i < cached_collapsed_intervals_.size(); ++i) {
      running = std::max(running, cached_collapsed_intervals_[i].hi);
      cached_collapsed_hi_prefix_max_[i] = running;
    }
  }
  // Per-range prefix running-max of `closer_line` for InnermostFoldContaining.
  cached_range_closer_prefix_max_.resize(ranges_.size());
  {
    std::size_t running = 0;
    for (std::size_t i = 0; i < ranges_.size(); ++i) {
      running = std::max(running, ranges_[i].closer_line);
      cached_range_closer_prefix_max_[i] = running;
    }
  }
  cached_revision_ = revision_;
}

bool FoldingModel::ToggleFold(std::size_t opener_line) {
  const auto idx = IndexOfOpener(ranges_, opener_line);
  if (idx < 0) return false;
  const std::size_t pos = static_cast<std::size_t>(idx);
  const bool was_collapsed = collapsed_[pos];
  collapsed_[pos] = !was_collapsed;
  collapsed_count_ += was_collapsed ? std::size_t{0} : std::size_t{1};
  collapsed_count_ -= was_collapsed ? std::size_t{1} : std::size_t{0};
  ++revision_;
  return true;
}

bool FoldingModel::Collapse(std::size_t opener_line) {
  const auto idx = IndexOfOpener(ranges_, opener_line);
  if (idx < 0) return false;
  if (collapsed_[static_cast<std::size_t>(idx)]) return false;
  collapsed_[static_cast<std::size_t>(idx)] = true;
  ++collapsed_count_;
  ++revision_;
  return true;
}

bool FoldingModel::Expand(std::size_t opener_line) {
  const auto idx = IndexOfOpener(ranges_, opener_line);
  if (idx < 0) return false;
  if (!collapsed_[static_cast<std::size_t>(idx)]) return false;
  collapsed_[static_cast<std::size_t>(idx)] = false;
  --collapsed_count_;
  ++revision_;
  return true;
}

void FoldingModel::CollapseAll() {
  if (collapsed_count_ == collapsed_.size()) {
    return;
  }
  std::fill(collapsed_.begin(), collapsed_.end(), true);
  collapsed_count_ = collapsed_.size();
  ++revision_;
}

void FoldingModel::ExpandAll() {
  if (collapsed_count_ == 0) {
    return;
  }
  std::fill(collapsed_.begin(), collapsed_.end(), false);
  collapsed_count_ = 0;
  ++revision_;
}

bool FoldingModel::IsLineHidden(std::size_t line) const {
  EnsureLookupCache();
  if (cached_collapsed_intervals_.empty()) {
    return false;
  }
  // Find the largest interval index whose `lo` is <= line.
  const auto it = std::upper_bound(
      cached_collapsed_intervals_.begin(), cached_collapsed_intervals_.end(), line,
      [](std::size_t v, const CollapsedInterval& a) { return v < a.lo; });
  if (it == cached_collapsed_intervals_.begin()) {
    return false;
  }
  const std::size_t idx = static_cast<std::size_t>(it - cached_collapsed_intervals_.begin()) - 1;
  // The running-max of `hi` over [0..idx] tells us whether any prefix interval
  // covers `line`; since intervals are sorted by `lo`, lo <= line holds for the
  // whole prefix.
  return cached_collapsed_hi_prefix_max_[idx] >= line;
}

std::optional<FoldRange> FoldingModel::FoldStartingAt(std::size_t line) const {
  const auto idx = IndexOfOpener(ranges_, line);
  if (idx < 0) {
    return std::nullopt;
  }
  return ranges_[static_cast<std::size_t>(idx)];
}

std::optional<FoldRange> FoldingModel::InnermostFoldContaining(std::size_t line) const {
  if (ranges_.empty()) {
    return std::nullopt;
  }
  // Find the rightmost range whose opener_line <= line.
  const auto it = std::upper_bound(
      ranges_.begin(), ranges_.end(), line,
      [](std::size_t v, const FoldRange& r) { return v < r.opener_line; });
  if (it == ranges_.begin()) {
    return std::nullopt;
  }
  EnsureLookupCache();
  // Walk left checking closer_line >= line. The prefix running-max of closer
  // lets us early-exit: if max closer in [0..i] is < line, no fold in that
  // prefix can contain `line`.
  for (std::ptrdiff_t i = (it - ranges_.begin()) - 1; i >= 0; --i) {
    const std::size_t idx = static_cast<std::size_t>(i);
    if (cached_range_closer_prefix_max_[idx] < line) {
      return std::nullopt;
    }
    if (ranges_[idx].closer_line >= line) {
      return ranges_[idx];
    }
  }
  return std::nullopt;
}

void FoldingModel::AppendFoldsContaining(std::size_t line,
                                          std::vector<FoldRange>* out) const {
  if (out == nullptr || ranges_.empty()) {
    return;
  }
  const auto it = std::upper_bound(
      ranges_.begin(), ranges_.end(), line,
      [](std::size_t v, const FoldRange& r) { return v < r.opener_line; });
  if (it == ranges_.begin()) {
    return;
  }
  EnsureLookupCache();
  // Collect into a local buffer first so we can reverse to outermost-first.
  std::vector<FoldRange> ancestors;
  ancestors.reserve(8);
  for (std::ptrdiff_t i = (it - ranges_.begin()) - 1; i >= 0; --i) {
    const std::size_t idx = static_cast<std::size_t>(i);
    if (cached_range_closer_prefix_max_[idx] < line) {
      break;
    }
    if (ranges_[idx].closer_line >= line) {
      ancestors.push_back(ranges_[idx]);
    }
  }
  // ancestors is in decreasing opener_line order; reverse to outer-first.
  for (auto rit = ancestors.rbegin(); rit != ancestors.rend(); ++rit) {
    out->push_back(*rit);
  }
}

bool FoldingModel::IsCollapsedAtOpener(std::size_t line) const {
  const auto idx = IndexOfOpener(ranges_, line);
  if (idx < 0) {
    return false;
  }
  return collapsed_[static_cast<std::size_t>(idx)];
}

void FoldingModel::Clear() {
  ranges_.clear();
  collapsed_.clear();
  collapsed_count_ = 0;
  complete_ = true;
  dirty_ = true;
  resolved_prefix_line_count_ = 0;
  ++revision_;
}

}  // namespace microide::editor
