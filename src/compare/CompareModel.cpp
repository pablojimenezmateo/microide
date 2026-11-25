#include "compare/CompareModel.h"
#include "compare/CompareMoves.h"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "compare/HunkRefinement.h"
#include "util/FlatDedupSet.h"
#include "util/Fnv1a.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/SaturatingMath.h"
#include "util/StringUtil.h"

namespace microide::compare {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxLineLcsMatrixCells = 250'000;
bool ProductExceeds(std::size_t left, std::size_t right, std::size_t limit) {
  if (left == 0 || right == 0) {
    return false;
  }
  return left > limit / right;
}

std::uint64_t DurationNs(Clock::time_point start, Clock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

std::size_t SignificantBytes(std::string_view text) {
  std::size_t total = 0;
  for (const char byte : text) {
    total += util::IsAsciiSpace(static_cast<unsigned char>(byte)) != 0 ? 0 : 1;
  }
  return total;
}

std::size_t LineMatchWeight(std::string_view text,
                            std::size_t significant_bytes,
                            std::size_t occurrences) {
  const std::size_t informative_bytes =
      significant_bytes > 0 ? significant_bytes : (text.empty() ? 1 : std::size_t{2});
  const std::size_t rarity = std::max<std::size_t>(1, occurrences);
  return std::max<std::size_t>(1, (informative_bytes + 8) / rarity);
}

struct HunkScratch {
  std::vector<std::string_view> deleted_lines;
  std::vector<std::string_view> inserted_lines;
  std::vector<HunkRow> rows;
};

HunkScratch& HunkScratchForThread() {
  thread_local HunkScratch scratch;
  return scratch;
}


void AppendDeleteOps(std::span<const std::string_view> lines,
                     std::size_t begin,
                     std::size_t end,
                     std::vector<DiffOp>& ops) {
  for (std::size_t i = begin; i < end; ++i) {
    ops.push_back(DiffOp{DiffOpKind::Delete, lines[i]});
  }
}

void AppendInsertOps(std::span<const std::string_view> lines,
                     std::size_t begin,
                     std::size_t end,
                     std::vector<DiffOp>& ops) {
  for (std::size_t i = begin; i < end; ++i) {
    ops.push_back(DiffOp{DiffOpKind::Insert, lines[i]});
  }
}

bool LinesEqualForDiff(std::string_view left,
                       std::string_view right,
                       const CompareBuildOptions& options);
bool LinesEqualIgnoringWhitespace(std::string_view left, std::string_view right);

}

namespace detail {

struct IgnoreWhitespaceLineHash {
  std::size_t operator()(std::string_view line) const {
    std::uint64_t hash = util::kFnv1aOffsetBasis;
    for (const char c : line) {
      if (util::IsAsciiSpace(static_cast<unsigned char>(c)) != 0) {
        continue;
      }
      hash = util::Fnv1aByte(hash, static_cast<unsigned char>(c));
    }
    return static_cast<std::size_t>(hash);
  }
};

struct IgnoreWhitespaceLineEq {
  bool operator()(std::string_view left, std::string_view right) const {
    return LinesEqualIgnoringWhitespace(left, right);
  }
};

struct AnchorInfo {
  std::size_t left_count = 0;
  std::size_t right_count = 0;
  std::size_t left_index = 0;
  std::size_t right_index = 0;
};

struct DiffScratch {
  std::vector<std::uint32_t> left_ids;
  std::vector<std::uint32_t> right_ids;
  std::vector<std::uint32_t> class_occurrences;
  std::vector<std::size_t> left_match_weight;
  std::vector<std::size_t> dp;
  util::FlatDedupSet<std::string_view> line_ids{0};
  util::FlatDedupSet<std::string_view, IgnoreWhitespaceLineHash, IgnoreWhitespaceLineEq>
      whitespace_insensitive_line_ids{0};
  std::vector<AnchorInfo> info_by_id;
  std::vector<std::pair<std::size_t, std::size_t>> candidates;
  std::vector<std::size_t> pile_tops;
  std::vector<std::size_t> pile_candidate_indices;
  std::vector<std::size_t> predecessor;
  std::vector<std::vector<std::pair<std::size_t, std::size_t>>> anchors_by_depth;
};

struct CompareBuildScratch {
  std::vector<std::string_view> left_lines;
  std::vector<std::string_view> right_lines;
  std::vector<DiffOp> ops;
  DiffScratch diff;
  HunkRefinementCache hunk_refinements;
};

}

namespace {

using detail::AnchorInfo;
using detail::DiffScratch;

void AppendEqualPairs(std::span<const std::string_view> left_lines,
                      std::size_t left_begin,
                      std::size_t left_end,
                      std::span<const std::string_view> right_lines,
                      std::size_t right_begin,
                      std::vector<DiffOp>& ops) {
  for (std::size_t offset = 0; left_begin + offset < left_end; ++offset) {
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[left_begin + offset],
                         right_lines[right_begin + offset]});
  }
}

void AppendExactLineOps(std::span<const std::string_view> left_lines,
                        std::span<const std::string_view> right_lines,
                        const CompareBuildOptions& options,
                        DiffScratch& scratch,
                        std::vector<DiffOp>& ops) {
  const std::size_t left_count = left_lines.size();
  const std::size_t right_count = right_lines.size();
  if (left_count == 0 && right_count == 0) {
    return;
  }

  std::vector<std::uint32_t>& left_ids = scratch.left_ids;
  std::vector<std::uint32_t>& right_ids = scratch.right_ids;
  std::vector<std::uint32_t>& class_occurrences = scratch.class_occurrences;
  left_ids.assign(left_count, 0);
  right_ids.assign(right_count, 0);
  class_occurrences.clear();
  class_occurrences.reserve(left_count + right_count);
  const auto assign_ids = [&](auto& ids) {
    ids.Reset(left_count + right_count);
    const auto intern = [&](std::string_view line) {
      const std::size_t id = ids.Intern(line);
      if (id == class_occurrences.size()) {
        class_occurrences.push_back(1);
      } else {
        ++class_occurrences[id];
      }
      return static_cast<std::uint32_t>(id);
    };
    for (std::size_t i = 0; i < left_count; ++i) left_ids[i] = intern(left_lines[i]);
    for (std::size_t j = 0; j < right_count; ++j) right_ids[j] = intern(right_lines[j]);
  };
  if (options.ignore_whitespace) {
    assign_ids(scratch.whitespace_insensitive_line_ids);
  } else {
    assign_ids(scratch.line_ids);
  }

  std::vector<std::size_t>& left_match_weight = scratch.left_match_weight;
  left_match_weight.assign(left_count, 1);
  for (std::size_t i = 0; i < left_count; ++i) {
    left_match_weight[i] = LineMatchWeight(left_lines[i], SignificantBytes(left_lines[i]),
                                           class_occurrences[left_ids[i]]);
  }

  const std::size_t stride = right_count + 1;
  std::vector<std::size_t>& dp = scratch.dp;
  dp.assign((left_count + 1) * stride, 0);
  auto at = [&](std::size_t i, std::size_t j) -> std::size_t& { return dp[i * stride + j]; };

  for (std::size_t i = left_count; i-- > 0;) {
    std::size_t* row = dp.data() + i * stride;
    const std::size_t* next_row = row + stride;
    const std::uint32_t left_id = left_ids[i];
    const std::size_t weight = left_match_weight[i];
    for (std::size_t j = right_count; j-- > 0;) {
      std::size_t best = std::max(next_row[j], row[j + 1]);
      if (left_id == right_ids[j]) {
        const std::size_t diagonal = weight + next_row[j + 1];
        if (diagonal > best) {
          best = diagonal;
        }
      }
      row[j] = best;
    }
  }

  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left_count && j < right_count) {
    if (left_ids[i] == right_ids[j]) {
      const std::size_t diagonal = left_match_weight[i] + at(i + 1, j + 1);
      if (diagonal >= at(i + 1, j) && diagonal >= at(i, j + 1) && at(i, j) == diagonal) {
        ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[i], right_lines[j]});
        ++i;
        ++j;
        continue;
      }
    }

    if (at(i + 1, j) >= at(i, j + 1)) {
      ops.push_back(DiffOp{DiffOpKind::Delete, left_lines[i]});
      ++i;
    } else {
      ops.push_back(DiffOp{DiffOpKind::Insert, right_lines[j]});
      ++j;
    }
  }
  AppendDeleteOps(left_lines, i, left_count, ops);
  AppendInsertOps(right_lines, j, right_count, ops);
}

void BuildUniqueLineAnchors(std::span<const std::string_view> left_lines,
                            std::size_t left_begin,
                            std::size_t left_end,
                            std::span<const std::string_view> right_lines,
                            std::size_t right_begin,
                            std::size_t right_end,
                            DiffScratch& scratch,
                            std::vector<std::pair<std::size_t, std::size_t>>& anchors) {
  anchors.clear();

  const std::size_t line_count = (left_end - left_begin) + (right_end - right_begin);
  util::FlatDedupSet<std::string_view>& line_ids = scratch.line_ids;
  line_ids.Reset(line_count);
  std::vector<AnchorInfo>& info_by_id = scratch.info_by_id;
  info_by_id.clear();
  info_by_id.reserve(line_count);
  const auto info_for = [&](std::string_view line) -> AnchorInfo& {
    const std::size_t id = line_ids.Intern(line);
    if (id == info_by_id.size()) {
      info_by_id.emplace_back();
    }
    return info_by_id[id];
  };
  for (std::size_t i = left_begin; i < left_end; ++i) {
    AnchorInfo& info = info_for(left_lines[i]);
    ++info.left_count;
    info.left_index = i;
  }
  for (std::size_t i = right_begin; i < right_end; ++i) {
    AnchorInfo& info = info_for(right_lines[i]);
    ++info.right_count;
    info.right_index = i;
  }

  std::vector<std::pair<std::size_t, std::size_t>>& candidates = scratch.candidates;
  candidates.clear();
  candidates.reserve(std::min(left_end - left_begin, right_end - right_begin));
  for (std::size_t id = 0; id < info_by_id.size(); ++id) {
    const AnchorInfo& info = info_by_id[id];
    if (line_ids.keys()[id].empty()) {
      continue;
    }
    if (info.left_count == 1 && info.right_count == 1) {
      candidates.emplace_back(info.left_index, info.right_index);
    }
  }
  std::sort(candidates.begin(), candidates.end());
  if (candidates.empty()) {
    return;
  }

  const std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t>& pile_tops = scratch.pile_tops;
  std::vector<std::size_t>& pile_candidate_indices = scratch.pile_candidate_indices;
  std::vector<std::size_t>& predecessor = scratch.predecessor;
  pile_tops.clear();
  pile_candidate_indices.clear();
  predecessor.assign(candidates.size(), kNoIndex);
  for (std::size_t candidate_index = 0; candidate_index < candidates.size(); ++candidate_index) {
    const std::size_t right_index = candidates[candidate_index].second;
    const auto it = std::lower_bound(pile_tops.begin(), pile_tops.end(), right_index);
    const std::size_t pile_index = static_cast<std::size_t>(it - pile_tops.begin());
    if (pile_index > 0) {
      predecessor[candidate_index] = pile_candidate_indices[pile_index - 1];
    }
    if (it == pile_tops.end()) {
      pile_tops.push_back(right_index);
      pile_candidate_indices.push_back(candidate_index);
    } else {
      *it = right_index;
      pile_candidate_indices[pile_index] = candidate_index;
    }
  }

  if (pile_candidate_indices.empty()) {
    return;
  }
  for (std::size_t index = pile_candidate_indices.back(); index != kNoIndex;
       index = predecessor[index]) {
    anchors.push_back(candidates[index]);
  }
  std::reverse(anchors.begin(), anchors.end());
}

constexpr std::size_t kMaxAnchoredFallbackDepth = 256;

std::vector<std::pair<std::size_t, std::size_t>>& AnchorsAtDepth(DiffScratch& scratch,
                                                                 std::size_t depth) {
  if (scratch.anchors_by_depth.size() <= kMaxAnchoredFallbackDepth) {
    scratch.anchors_by_depth.resize(kMaxAnchoredFallbackDepth + 1);
  }
  return scratch.anchors_by_depth[depth];
}

void AppendAnchoredFallbackOps(std::span<const std::string_view> left_lines,
                               std::size_t left_begin,
                               std::size_t left_end,
                               std::span<const std::string_view> right_lines,
                               std::size_t right_begin,
                               std::size_t right_end,
                               const CompareBuildOptions& options,
                               DiffScratch& scratch,
                               std::vector<DiffOp>& ops,
                               std::size_t depth) {
  while (left_begin < left_end && right_begin < right_end &&
         LinesEqualForDiff(left_lines[left_begin], right_lines[right_begin], options)) {
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[left_begin], right_lines[right_begin]});
    ++left_begin;
    ++right_begin;
  }

  std::size_t left_suffix = left_end;
  std::size_t right_suffix = right_end;
  while (left_suffix > left_begin && right_suffix > right_begin &&
         LinesEqualForDiff(left_lines[left_suffix - 1], right_lines[right_suffix - 1], options)) {
    --left_suffix;
    --right_suffix;
  }

  if (left_begin == left_suffix && right_begin == right_suffix) {
    AppendEqualPairs(left_lines, left_suffix, left_end, right_lines, right_suffix, ops);
    return;
  }
  if (left_begin == left_suffix) {
    AppendInsertOps(right_lines, right_begin, right_suffix, ops);
    AppendEqualPairs(left_lines, left_suffix, left_end, right_lines, right_suffix, ops);
    return;
  }
  if (right_begin == right_suffix) {
    AppendDeleteOps(left_lines, left_begin, left_suffix, ops);
    AppendEqualPairs(left_lines, left_suffix, left_end, right_lines, right_suffix, ops);
    return;
  }

  const std::size_t middle_left_count = left_suffix - left_begin;
  const std::size_t middle_right_count = right_suffix - right_begin;
  if (!ProductExceeds(middle_left_count + 1, middle_right_count + 1, kMaxLineLcsMatrixCells)) {
    const std::span<const std::string_view> left_slice =
        left_lines.subspan(left_begin, left_suffix - left_begin);
    const std::span<const std::string_view> right_slice =
        right_lines.subspan(right_begin, right_suffix - right_begin);
    AppendExactLineOps(left_slice, right_slice, options, scratch, ops);
    AppendEqualPairs(left_lines, left_suffix, left_end, right_lines, right_suffix, ops);
    return;
  }

  if (depth >= kMaxAnchoredFallbackDepth) {
    AppendDeleteOps(left_lines, left_begin, left_suffix, ops);
    AppendInsertOps(right_lines, right_begin, right_suffix, ops);
    AppendEqualPairs(left_lines, left_suffix, left_end, right_lines, right_suffix, ops);
    return;
  }

  std::vector<std::pair<std::size_t, std::size_t>>& anchors = AnchorsAtDepth(scratch, depth);
  BuildUniqueLineAnchors(left_lines, left_begin, left_suffix, right_lines, right_begin,
                         right_suffix, scratch, anchors);
  if (anchors.empty()) {
    AppendDeleteOps(left_lines, left_begin, left_suffix, ops);
    AppendInsertOps(right_lines, right_begin, right_suffix, ops);
    AppendEqualPairs(left_lines, left_suffix, left_end, right_lines, right_suffix, ops);
    return;
  }

  std::size_t segment_left_begin = left_begin;
  std::size_t segment_right_begin = right_begin;
  for (const auto& [anchor_left, anchor_right] : anchors) {
    AppendAnchoredFallbackOps(left_lines, segment_left_begin, anchor_left, right_lines,
                              segment_right_begin, anchor_right, options, scratch, ops, depth + 1);
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[anchor_left], right_lines[anchor_right]});
    segment_left_begin = anchor_left + 1;
    segment_right_begin = anchor_right + 1;
  }
  AppendAnchoredFallbackOps(left_lines, segment_left_begin, left_suffix, right_lines,
                            segment_right_begin, right_suffix, options, scratch, ops, depth + 1);
  AppendEqualPairs(left_lines, left_suffix, left_end, right_lines, right_suffix, ops);
}

}

namespace {

bool LinesEqualIgnoringWhitespace(std::string_view left, std::string_view right) {
  std::size_t left_index = 0;
  std::size_t right_index = 0;
  while (left_index < left.size() || right_index < right.size()) {
    while (left_index < left.size() && util::IsAsciiSpace(static_cast<unsigned char>(left[left_index])) != 0) {
      ++left_index;
    }
    while (right_index < right.size() &&
           util::IsAsciiSpace(static_cast<unsigned char>(right[right_index])) != 0) {
      ++right_index;
    }
    if (left_index >= left.size() || right_index >= right.size()) {
      return left_index >= left.size() && right_index >= right.size();
    }
    if (left[left_index] != right[right_index]) {
      return false;
    }
    ++left_index;
    ++right_index;
  }
  return true;
}

bool LinesEqualForDiff(std::string_view left,
                       std::string_view right,
                       const CompareBuildOptions& options) {
  if (left == right) {
    return true;
  }
  return options.ignore_whitespace && LinesEqualIgnoringWhitespace(left, right);
}

struct EqualEnds {
  std::size_t prefix = 0;
  std::size_t left_suffix = 0;
  std::size_t right_suffix = 0;
};

EqualEnds FindEqualEnds(std::span<const std::string_view> left_lines,
                        std::span<const std::string_view> right_lines,
                        const CompareBuildOptions& options) {
  EqualEnds ends;
  while (ends.prefix < left_lines.size() && ends.prefix < right_lines.size() &&
         LinesEqualForDiff(left_lines[ends.prefix], right_lines[ends.prefix], options)) {
    ++ends.prefix;
  }
  ends.left_suffix = left_lines.size();
  ends.right_suffix = right_lines.size();
  while (ends.left_suffix > ends.prefix && ends.right_suffix > ends.prefix &&
         LinesEqualForDiff(left_lines[ends.left_suffix - 1], right_lines[ends.right_suffix - 1],
                           options)) {
    --ends.left_suffix;
    --ends.right_suffix;
  }
  return ends;
}

}

namespace {

CompareSideChange SideChange(const CompareRow& row, int line,
                             const std::vector<CompareTextSpan>& spans, bool moved) {
  if (line == 0 || row.kind == CompareRowKind::Unchanged) {
    return CompareSideChange::None;
  }
  if (spans.empty()) {
    return CompareSideChange::LayoutOnly;
  }
  return moved ? CompareSideChange::Moved : CompareSideChange::Novel;
}

}

CompareSideChange LeftSideChange(const CompareRow& row) {
  return SideChange(row, row.left_line, row.left_changed_spans, row.left_moved);
}

CompareSideChange RightSideChange(const CompareRow& row) {
  return SideChange(row, row.right_line, row.right_changed_spans, row.right_moved);
}

bool CompareRowIsLayoutOnly(const CompareRow& row) {
  return row.kind != CompareRowKind::Unchanged && row.left_changed_spans.empty() &&
         row.right_changed_spans.empty();
}

const CompareTextBuffer& EmptyCompareText() {
  static const CompareTextBuffer empty = std::make_shared<const std::string>();
  return empty;
}

CompareTextBuffer MakeCompareText(std::string text) {
  if (text.empty()) {
    return EmptyCompareText();
  }
  return std::make_shared<const std::string>(std::move(text));
}

std::vector<DiffOp> BuildLineDiffOps(std::span<const std::string_view> left_lines,
                                     std::span<const std::string_view> right_lines,
                                     LineDiffBuildStats* stats) {
  return BuildLineDiffOps(left_lines, right_lines, CompareBuildOptions{}, stats);
}

std::vector<DiffOp> BuildLineDiffOps(std::span<const std::string_view> left_lines,
                                     std::span<const std::string_view> right_lines,
                                     const CompareBuildOptions& options,
                                     LineDiffBuildStats* stats) {
  std::vector<DiffOp> ops;
  detail::DiffScratch scratch;
  BuildLineDiffOpsInto(left_lines, right_lines, options, scratch, ops, stats);
  return ops;
}

void BuildLineDiffOpsInto(std::span<const std::string_view> left_lines,
                          std::span<const std::string_view> right_lines,
                          const CompareBuildOptions& options,
                          detail::DiffScratch& scratch,
                          std::vector<DiffOp>& ops,
                          LineDiffBuildStats* stats) {
  const auto [prefix, left_suffix, right_suffix] = FindEqualEnds(left_lines, right_lines, options);

  ops.clear();
  ops.reserve(left_lines.size() + right_lines.size());
  for (std::size_t index = 0; index < prefix; ++index) {
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[index], right_lines[index]});
  }

  const std::span<const std::string_view> left_middle =
      left_lines.subspan(prefix, left_suffix - prefix);
  const std::span<const std::string_view> right_middle =
      right_lines.subspan(prefix, right_suffix - prefix);
  if (!left_middle.empty() || !right_middle.empty()) {
    const std::size_t left_count = left_middle.size();
    const std::size_t right_count = right_middle.size();
    if (ProductExceeds(left_count + 1, right_count + 1, kMaxLineLcsMatrixCells)) {
      if (stats != nullptr) {
        ++stats->anchored_alignment_calls;
      }
      AppendAnchoredFallbackOps(left_middle, 0, left_middle.size(), right_middle, 0,
                                right_middle.size(), options, scratch, ops, 0);
    } else {
      if (stats != nullptr) {
        ++stats->exact_alignment_calls;
      }
      AppendExactLineOps(left_middle, right_middle, options, scratch, ops);
    }
  }

  for (std::size_t index = left_suffix; index < left_lines.size(); ++index) {
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[index],
                         right_lines[index - left_suffix + right_suffix]});
  }
}

CompareBuildResult BuildCompareModelProfiled(const std::string& left, const std::string& right) {
  return BuildCompareModelProfiled(left, right, CompareBuildOptions{});
}

namespace {

class RowSink {
 public:
  explicit RowSink(CompareModel& model) : model_(model) {}

  CompareRow& Next() {
    if (next_ == model_.rows.size()) {
      model_.rows.emplace_back();
    }
    CompareRow& row = model_.rows[next_++];
    row.left_text = {};
    row.right_text = {};
    row.left_line = 0;
    row.right_line = 0;
    row.kind = CompareRowKind::Unchanged;
    row.hunk = -1;
    row.left_changed_spans.clear();
    row.right_changed_spans.clear();
    row.left_moved = false;
    row.right_moved = false;
    return row;
  }

  std::size_t size() const { return next_; }
  void Finish() { model_.rows.resize(next_); }

 private:
  CompareModel& model_;
  std::size_t next_ = 0;
};

}

namespace detail {

void DestroyCompareBuildScratch(CompareBuildScratch* scratch) noexcept {
  delete scratch;
}

}

namespace {

detail::CompareBuildScratch& EnsureBuildScratch(CompareModel& model) {
  if (model.build_scratch.get() == nullptr) {
    model.build_scratch.reset(new detail::CompareBuildScratch());
  }
  return *model.build_scratch.get();
}

}

static CompareBuildProfile BuildCompareModelProfiledInto(CompareModel& model,
                                                         CompareTextBuffer left_buffer,
                                                         CompareTextBuffer right_buffer,
                                                         const CompareBuildOptions& options) {
  CompareBuildProfile profile;
  for (CompareRow& row : model.rows) {
    row.left_text = {};
    row.right_text = {};
  }
  model.hunks.clear();
  if (left_buffer != nullptr) {
    model.left_source = std::move(left_buffer);
  } else {
    model.left_source = EmptyCompareText();
  }
  if (right_buffer != nullptr) {
    model.right_source = std::move(right_buffer);
  } else {
    model.right_source = EmptyCompareText();
  }
  const std::string& left = *model.left_source;
  const std::string& right = *model.right_source;

  model.left_final_newline_missing = !left.empty() && left.back() != '\n';
  model.right_final_newline_missing = !right.empty() && right.back() != '\n';
  model.left_absent = options.left_exists.has_value() ? !*options.left_exists : left.empty();
  model.right_absent = options.right_exists.has_value() ? !*options.right_exists : right.empty();
  const auto uses_crlf = [](const std::string& buffer) {
    const std::size_t newline = buffer.find('\n');
    return newline != std::string::npos && newline > 0 && buffer[newline - 1] == '\r';
  };
  model.left_uses_crlf = uses_crlf(left);
  model.right_uses_crlf = uses_crlf(right);

  const Clock::time_point total_start = Clock::now();

  const Clock::time_point split_start = Clock::now();
  detail::CompareBuildScratch& scratch = EnsureBuildScratch(model);
  util::SplitLineViewsInto(left, scratch.left_lines);
  util::SplitLineViewsInto(right, scratch.right_lines);
  const bool left_phantom = !model.left_final_newline_missing;
  const bool right_phantom = !model.right_final_newline_missing;
  if (left_phantom && !scratch.left_lines.empty() && scratch.left_lines.back().empty()) {
    scratch.left_lines.pop_back();
  }
  if (right_phantom && !scratch.right_lines.empty() && scratch.right_lines.back().empty()) {
    scratch.right_lines.pop_back();
  }
  const std::vector<std::string_view>& left_lines = scratch.left_lines;
  const std::vector<std::string_view>& right_lines = scratch.right_lines;
  profile.split_lines_ns = DurationNs(split_start, Clock::now());

  const Clock::time_point line_alignment_start = Clock::now();
  const auto [prefix, left_suffix, right_suffix] = FindEqualEnds(left_lines, right_lines, options);

  const std::span<const std::string_view> left_middle =
      std::span<const std::string_view>(left_lines).subspan(prefix, left_suffix - prefix);
  const std::span<const std::string_view> right_middle =
      std::span<const std::string_view>(right_lines).subspan(prefix, right_suffix - prefix);

  LineDiffBuildStats line_diff_stats;
  BuildLineDiffOpsInto(left_middle, right_middle, options, scratch.diff, scratch.ops,
                       &line_diff_stats);
  const std::vector<DiffOp>& ops = scratch.ops;
  profile.exact_line_alignment_calls += line_diff_stats.exact_alignment_calls;
  profile.anchored_line_alignment_calls += line_diff_stats.anchored_alignment_calls;
  profile.line_alignment_ns = DurationNs(line_alignment_start, Clock::now());

  const std::shared_ptr<const DiffSyntax>& syntax =
      options.syntax != nullptr ? options.syntax : GenericDiffSyntax();
  HunkRefinementStats refinement_stats;
  scratch.hunk_refinements.BeginBuild(syntax);
  struct SideScan {
    DiffLexState state;
    std::size_t line = 0;
  };
  SideScan left_scan;
  SideScan right_scan;
  const auto state_at = [&syntax](const std::vector<std::string_view>& lines, SideScan& scan,
                                  std::size_t line) {
    if (line > scan.line) {
      scan.state = ScanDiffLexState(std::span<const std::string_view>(lines).subspan(scan.line),
                                    line - scan.line, *syntax, scan.state);
      scan.line = line;
    }
    return scan.state;
  };
  model.rows.reserve(left_lines.size() + right_lines.size());
  RowSink sink(model);
  int left_line = 1;
  int right_line = 1;
  for (std::size_t index = 0; index < prefix; ++index) {
    CompareRow& row = sink.Next();
    row.left_text = left_lines[index];
    row.right_text = right_lines[index];
    row.left_line = left_line++;
    row.right_line = right_line++;
  }

  for (std::size_t op_index = 0; op_index < ops.size(); ++op_index) {
    const auto& op = ops[op_index];
    if (op.kind == DiffOpKind::Equal) {
      CompareRow& row = sink.Next();
      row.left_text = op.text;
      row.right_text = op.right_text;
      row.left_line = left_line++;
      row.right_line = right_line++;
      continue;
    }

    const int hunk_start = static_cast<int>(sink.size());
    HunkScratch& hunk_scratch = HunkScratchForThread();
    std::vector<std::string_view>& deleted_lines = hunk_scratch.deleted_lines;
    std::vector<std::string_view>& inserted_lines = hunk_scratch.inserted_lines;
    deleted_lines.clear();
    inserted_lines.clear();
    while (op_index < ops.size() && ops[op_index].kind != DiffOpKind::Equal) {
      if (ops[op_index].kind == DiffOpKind::Delete) {
        deleted_lines.push_back(ops[op_index].text);
      } else if (ops[op_index].kind == DiffOpKind::Insert) {
        inserted_lines.push_back(ops[op_index].text);
      }
      ++op_index;
    }
    --op_index;

    const int hunk_index = static_cast<int>(model.hunks.size());
    const Clock::time_point refine_start = Clock::now();
    HunkRefinementView refinement;
    if (options.refine_hunks) {
      const DiffLexState left_entry =
          state_at(left_lines, left_scan, static_cast<std::size_t>(left_line - 1));
      const DiffLexState right_entry =
          state_at(right_lines, right_scan, static_cast<std::size_t>(right_line - 1));
      refinement = scratch.hunk_refinements.Refine(deleted_lines, inserted_lines, &refinement_stats,
                                                   left_entry, right_entry);
    } else {
      ZipHunk(deleted_lines.size(), inserted_lines.size(), hunk_scratch.rows);
      refinement = HunkRefinementView(hunk_scratch.rows, {}, {}, 0);
    }
    profile.hunk_refinement_ns += DurationNs(refine_start, Clock::now());

    const int left_base = left_line;
    const int right_base = right_line;
    for (const HunkRow& hunk_row : refinement.rows()) {
      CompareRow& compare_row = sink.Next();
      compare_row.hunk = hunk_index;
      if (hunk_row.left != kNoHunkLine) {
        compare_row.left_text = deleted_lines[hunk_row.left];
        compare_row.left_line = left_base + static_cast<int>(hunk_row.left);
        const std::span<const CompareTextSpan> spans = refinement.LeftSpans(hunk_row.left);
        compare_row.left_changed_spans.assign(spans.begin(), spans.end());
      }
      if (hunk_row.right != kNoHunkLine) {
        compare_row.right_text = inserted_lines[hunk_row.right];
        compare_row.right_line = right_base + static_cast<int>(hunk_row.right);
        const std::span<const CompareTextSpan> spans = refinement.RightSpans(hunk_row.right);
        compare_row.right_changed_spans.assign(spans.begin(), spans.end());
      }
      if (compare_row.left_line != 0 && compare_row.right_line != 0) {
        compare_row.kind = CompareRowKind::Modified;
      } else if (compare_row.left_line != 0) {
        compare_row.kind = CompareRowKind::Deleted;
      } else {
        compare_row.kind = CompareRowKind::Added;
      }
    }
    left_line += static_cast<int>(deleted_lines.size());
    right_line += static_cast<int>(inserted_lines.size());

    model.hunks.push_back(CompareHunk{
        .index = hunk_index,
        .start_row = hunk_start,
        .end_row = static_cast<int>(sink.size()) - 1,
    });
  }

  for (std::size_t index = left_suffix; index < left_lines.size(); ++index) {
    CompareRow& row = sink.Next();
    row.left_text = left_lines[index];
    row.right_text = right_lines[index - left_suffix + right_suffix];
    row.left_line = left_line++;
    row.right_line = right_line++;
  }
  if (left_phantom || right_phantom) {
    CompareRow& row = sink.Next();
    row.left_text = std::string_view();
    row.right_text = std::string_view();
    if (left_phantom && right_phantom) {
      row.left_line = left_line++;
      row.right_line = right_line++;
    } else {
      row.kind = left_phantom ? CompareRowKind::Deleted : CompareRowKind::Added;
      row.left_line = left_phantom ? left_line++ : 0;
      row.right_line = right_phantom ? right_line++ : 0;
      const int row_index = static_cast<int>(sink.size()) - 1;
      if (!model.hunks.empty() && model.hunks.back().end_row == row_index - 1) {
        model.hunks.back().end_row = row_index;
      } else {
        model.hunks.push_back(CompareHunk{
            .index = static_cast<int>(model.hunks.size()),
            .start_row = row_index,
            .end_row = row_index,
        });
      }
      row.hunk = model.hunks.back().index;
    }
  }
  sink.Finish();
  if (options.refine_hunks) {
    MarkMovedLines(model);
  }

  profile.total_ns = DurationNs(total_start, Clock::now());
  scratch.hunk_refinements.EndBuild();
  profile.structural_hunks = refinement_stats.structural_hunks;
  profile.cached_hunks = refinement_stats.cached_hunks;
  profile.flat_fallback_hunks = refinement_stats.flat_fallback_hunks;
  profile.graph_vertices = refinement_stats.graph_vertices;
  const std::uint64_t measured_substeps =
      profile.split_lines_ns + profile.line_alignment_ns + profile.hunk_refinement_ns;
  profile.row_assembly_ns = util::SaturatingSub(profile.total_ns, measured_substeps);

  util::PerformanceTrace::RecordSampleNs("compare::Build::SplitLines", profile.split_lines_ns);
  util::PerformanceTrace::RecordSampleNs("compare::Build::LineAlignment",
                                         profile.line_alignment_ns);
  util::PerformanceTrace::RecordSampleNs("compare::Build::HunkRefinement",
                                         profile.hunk_refinement_ns);
  util::PerformanceTrace::RecordSampleNs("compare::Build::RowAssembly", profile.row_assembly_ns);

  util::AddPerformanceCounter(util::PerfCounterId::CompareModelBuilds);
  util::AddPerformanceCounter(util::PerfCounterId::CompareModelInputLines,
                              left_lines.size() + right_lines.size());
  util::AddPerformanceCounter(util::PerfCounterId::CompareModelRowsProduced, model.rows.size());
  util::AddPerformanceCounter(util::PerfCounterId::CompareStructuralHunks,
                              profile.structural_hunks + profile.flat_fallback_hunks);
  util::AddPerformanceCounter(util::PerfCounterId::CompareStructuralGraphVertices,
                              profile.graph_vertices);
  return profile;
}

CompareBuildResult BuildCompareModelProfiled(const std::string& left,
                                             const std::string& right,
                                             const CompareBuildOptions& options) {
  CompareBuildResult result;
  result.profile = BuildCompareModelProfiledInto(result.model, MakeCompareText(left),
                                                 MakeCompareText(right), options);
  return result;
}

CompareModel BuildCompareModel(const std::string& left, const std::string& right) {
  return BuildCompareModelProfiled(left, right).model;
}

CompareBuildProfile BuildCompareModelInto(CompareModel& model,
                                          CompareTextBuffer left,
                                          CompareTextBuffer right,
                                          const CompareBuildOptions& options) {
  return BuildCompareModelProfiledInto(model, std::move(left), std::move(right), options);
}

CompareBuildProfile BuildCompareModelInto(CompareModel& model,
                                          const std::string& left,
                                          const std::string& right,
                                          const CompareBuildOptions& options) {
  return BuildCompareModelProfiledInto(model, MakeCompareText(left), MakeCompareText(right),
                                       options);
}

CompareModel BuildCompareModel(const std::string& left,
                               const std::string& right,
                               const CompareBuildOptions& options) {
  return BuildCompareModelProfiled(left, right, options).model;
}

}
