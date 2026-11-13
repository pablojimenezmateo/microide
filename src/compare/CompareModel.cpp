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

// The non-whitespace bytes of a line: how much a line match "says". A blank or
// brace-only line is weak evidence that two regions correspond.
std::size_t SignificantBytes(std::string_view text) {
  std::size_t total = 0;
  for (const char byte : text) {
    total += util::IsAsciiSpace(static_cast<unsigned char>(byte)) != 0 ? 0 : 1;
  }
  return total;
}

// How much matching this line is worth to the line aligner: informative lines
// weigh more, and a line that occurs many times weighs less, so the alignment
// anchors on distinctive lines rather than on runs of `}` or blank lines.
std::size_t LineMatchWeight(std::string_view text,
                            std::size_t significant_bytes,
                            std::size_t occurrences) {
  const std::size_t informative_bytes =
      significant_bytes > 0 ? significant_bytes : (text.empty() ? 1 : std::size_t{2});
  const std::size_t rarity = std::max<std::size_t>(1, occurrences);
  return std::max<std::size_t>(1, (informative_bytes + 8) / rarity);
}

// Per-thread scratch for assembling one hunk: its two line runs and, for an
// unrefined build, its rows. Grown once and reused for every hunk
// of every build on the thread.
struct HunkScratch {
  std::vector<std::string_view> deleted_lines;
  std::vector<std::string_view> inserted_lines;
  // Rows of an unrefined (line-only) hunk.
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

// Defined further down in this file; declared here so the line-op builders can
// honour ignore_whitespace inside changed hunks, not just on the matched edges.
bool LinesEqualForDiff(std::string_view left,
                       std::string_view right,
                       const CompareBuildOptions& options);
bool LinesEqualIgnoringWhitespace(std::string_view left, std::string_view right);

}  // namespace

// These four types are members of `detail::CompareBuildScratch`, which the
// header declares — so they cannot live in the anonymous namespace, or the
// scratch would be an external-linkage class with internal-linkage fields
// (-Wsubobject-linkage). Everything that USES them stays file-local.
namespace detail {

// Whitespace-insensitive line identity, so the ignore_whitespace interning pass
// can key on the original view instead of a normalised copy of it. The hash
// walks the same bytes the equality does — everything that is not ASCII space —
// which is what makes it agree with LinesEqualIgnoringWhitespace.
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

// Per-line info for the unique-line anchor search, kept out of the function so
// DiffScratch can hold the vector it indexes.
struct AnchorInfo {
  std::size_t left_count = 0;
  std::size_t right_count = 0;
  std::size_t left_index = 0;
  std::size_t right_index = 0;
};

// Working buffers for one whole diff, threaded through the recursion.
//
// The anchored fallback recurses once per anchor segment, and each level used to
// build its own working set from scratch: the exact-LCS aligner allocated six
// vectors plus an intern table, the anchor search seven more. On the 12,000-line
// worktree diff the compare scenarios use — thirty hunks, so thirty-odd levels —
// that is a few hundred allocations per rebuild, and an editable compare pane
// rebuilds on EVERY keystroke.
//
// Every buffer here is dead by the time the function that filled it returns, so
// one instance is safe for the whole recursion even though levels nest. The one
// buffer that is NOT — the anchor list, which stays live while the level below
// it runs — is pooled BY DEPTH instead (`anchors_by_depth`), which gives it the
// same reuse without the aliasing.
struct DiffScratch {
  // Exact-LCS aligner.
  std::vector<std::uint32_t> left_ids;
  std::vector<std::uint32_t> right_ids;
  std::vector<std::uint32_t> class_occurrences;
  std::vector<std::size_t> left_match_weight;
  std::vector<std::size_t> dp;
  util::FlatDedupSet<std::string_view> line_ids{0};
  util::FlatDedupSet<std::string_view, IgnoreWhitespaceLineHash, IgnoreWhitespaceLineEq>
      whitespace_insensitive_line_ids{0};
  // Anchor search.
  std::vector<AnchorInfo> info_by_id;
  std::vector<std::pair<std::size_t, std::size_t>> candidates;
  std::vector<std::size_t> pile_tops;
  std::vector<std::size_t> pile_candidate_indices;
  std::vector<std::size_t> predecessor;
  // One anchor list per recursion depth. Level `d` reads its own list while
  // level `d + 1` runs, so unlike everything above it this cannot be a single
  // shared buffer — but a depth never collides with itself, so a pool works.
  // Sized once to the depth cap (see AnchorsAtDepth): a pool that GREW during
  // the recursion would move a live level's list out from under its loop.
  std::vector<std::vector<std::pair<std::size_t, std::size_t>>> anchors_by_depth;
};

// The buffers one whole rebuild needs, retained between rebuilds. See the
// declaration in `CompareModel.h` for why they live on the model.
struct CompareBuildScratch {
  std::vector<std::string_view> left_lines;
  std::vector<std::string_view> right_lines;
  std::vector<DiffOp> ops;
  DiffScratch diff;
  // Refinements of the previous build's hunks, so a rebuild refines only the
  // hunks that changed (one per keystroke in an editable pane).
  HunkRefinementCache hunk_refinements;
};

}  // namespace detail

namespace {

using detail::AnchorInfo;
using detail::DiffScratch;

// Emit a run of Equal ops carrying both columns' text for the lockstep-matched
// pair range [left_begin, left_end) <-> [right_begin, ...). The two ranges have
// equal length by construction at every caller.
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

  // Intern each line to an equality-class id before the DP. The table below is
  // bounded at kMaxLineLcsMatrixCells (250k) cells, and the loop used to call
  // LinesEqualForDiff at EVERY one of them — a full memcmp over the line, or
  // under ignore_whitespace a two-cursor whitespace-skipping walk. Hashing each
  // line once (O(N + M)) turns all of that into an integer compare. The ids
  // reproduce LinesEqualForDiff exactly because it is a genuine equivalence
  // relation either way: exact byte equality, or equality after removing every
  // whitespace byte (`==` implies the whitespace-insensitive form, so the
  // ignore_whitespace case is not a union of two relations).
  //
  // Interning is the ONLY hash structure this function builds now
  // (TD-2026-08-07-164). It used to build three: a `line_occurrences` map for
  // the rarity weight, plus one of two id maps — and under ignore_whitespace the
  // id map keyed on an owned, whitespace-stripped `std::string` per distinct
  // line, purely so `unordered_map` had something to hash. All three were a heap
  // node per distinct line, so a 14k-line compare paid ~28k node allocations
  // before the DP started. `FlatDedupSet` hands out ids that are dense indices
  // into its own key vector, so the occurrence counts are a plain vector indexed
  // by id, and a whitespace-insensitive Hash/Eq pair lets the ignore_whitespace
  // path key on the original view with no normalised copy at all.
  //
  // The counts are per equality CLASS, which under ignore_whitespace groups
  // lines that differ only in whitespace — the same relation the DP matches on,
  // so the rarity weight and the match it weighs now agree.
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

  // Row-pointer walk: `at()` recomputed `i * stride + j` for each of the four
  // accesses per cell. Hoisting the two rows out of the inner loop leaves one
  // add per access.
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

// Anchors are written into `anchors` rather than returned, because the caller's
// list stays live across the recursive calls it drives and so cannot come from
// the shared scratch. Everything else this needs does.
void BuildUniqueLineAnchors(std::span<const std::string_view> left_lines,
                            std::size_t left_begin,
                            std::size_t left_end,
                            std::span<const std::string_view> right_lines,
                            std::size_t right_begin,
                            std::size_t right_end,
                            DiffScratch& scratch,
                            std::vector<std::pair<std::size_t, std::size_t>>& anchors) {
  anchors.clear();

  // Same shape as AppendExactLineOps' interning, and the same reason: this ran on
  // the anchored fallback path — the one large diffs take — and cost a heap node
  // per distinct line before any anchor was found (TD-2026-08-07-164). Ids from
  // `Intern` are dense indices, so the per-line info is a parallel vector.
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

// See the depth-guard comment inside AppendAnchoredFallbackOps for why this cap
// exists; it is at file scope because the anchor pool is sized to it.
constexpr std::size_t kMaxAnchoredFallbackDepth = 256;

// The anchor list for one recursion level, from the depth pool.
//
// Sized to the whole cap on first use rather than grown per level, because a
// level holds a reference to its list across the calls that recurse below it —
// a pool that reallocated mid-recursion would move that list. 257 empty vectors
// is one allocation of ~6 KB, once per model.
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
  // The matched suffix [left_suffix, left_end) <-> [right_suffix, right_end) is
  // emitted (with both columns) by every return branch below.

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

  // Depth guard: in the worst case (e.g. N unique lines present in reversed order
  // on the two sides) the anchor recursion peels a single line per level, so an
  // adversarial merge/compare input from an untrusted repo could recurse ~N deep
  // -> stack overflow, plus O(N^2) CPU rebuilding the anchor map at each level.
  // Past the cap, emit the remaining middle as a coarse delete+insert (a correct,
  // just less-minimal, diff) instead of recursing further.
  if (depth >= kMaxAnchoredFallbackDepth) {
    AppendDeleteOps(left_lines, left_begin, left_suffix, ops);
    AppendInsertOps(right_lines, right_begin, right_suffix, ops);
    AppendEqualPairs(left_lines, left_suffix, left_end, right_lines, right_suffix, ops);
    return;
  }

  // Pooled by depth, not a local: this list stays live while the recursive calls
  // below run (so it cannot share the scratch's single-buffer members), but no
  // two live levels have the same depth. A fresh local here was ~10 allocations
  // and 350 KB per rebuild on the wrapped-diff fixture, and a compare pane
  // rebuilds on every keystroke (TD-2026-08-17-261).
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

}  // namespace

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

// The lines both sides share at the start and at the end: [0, prefix) match
// pairwise, and so do [left_suffix, end) and [right_suffix, end). The suffix
// never reaches into the prefix.
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

}  // namespace

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

}  // namespace

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
  // One object process-wide, so "no text" costs a refcount bump rather than an
  // allocation — a default-constructed CompareModel or compare tab hits this.
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
    // One scratch for the whole diff, supplied by the caller so it also survives
    // BETWEEN diffs. Both aligners used to build (and free) a whole `middle_ops`
    // vector here purely to copy it into `ops`, on top of the per-level buffers
    // inside them.
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

  // The matched suffix [left_suffix, end) <-> [right_suffix, end) has equal
  // length on both sides, so the right counterpart for left index `index` sits
  // at the same offset past right_suffix.
  for (std::size_t index = left_suffix; index < left_lines.size(); ++index) {
    ops.push_back(DiffOp{DiffOpKind::Equal, left_lines[index],
                         right_lines[index - left_suffix + right_suffix]});
  }
}

CompareBuildResult BuildCompareModelProfiled(const std::string& left, const std::string& right) {
  return BuildCompareModelProfiled(left, right, CompareBuildOptions{});
}

namespace {

// Row storage recycler.
//
// A compare rebuild used to construct every row from scratch, and `CompareRow`
// then owned TWO std::strings — so a 12,000-row diff cost 24,000 string
// allocations per rebuild, and the rebuild runs on every keystroke in the editable
// pane. That was 98 % of the allocations in `compare_type_in_wrapped_diff`
// (TD-2026-08-13-208).
//
// The rows of consecutive rebuilds are nearly identical, so this hands back the
// PREVIOUS build's row objects. Since TD-2026-08-14-232 a row's text is a view
// into the model's own source buffers rather than an owned string, so what is
// recycled is the row vector's storage and the two changed-span vectors' capacity.
// Rows past the new end are dropped at the end of the build; `resize` down never
// reallocates.
//
// It does NOT change what is built: every field is reset here, so a recycled row
// is indistinguishable from a fresh one.
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

}  // namespace

namespace detail {

void DestroyCompareBuildScratch(CompareBuildScratch* scratch) noexcept {
  delete scratch;
}

}  // namespace detail

namespace {

// Resolve the model's rebuild scratch, creating it on first use. One allocation
// per model, ever.
detail::CompareBuildScratch& EnsureBuildScratch(CompareModel& model) {
  if (model.build_scratch.get() == nullptr) {
    model.build_scratch.reset(new detail::CompareBuildScratch());
  }
  return *model.build_scratch.get();
}

}  // namespace

// Internal linkage: both callers are the two public entry points at the bottom of
// this file, and nothing declares it in the header. Left external it was the only
// -Wmissing-declarations hit in the tree, and it denied the optimizer the one
// thing worth having here -- this is the whole diff build, so whether it can be
// inlined into `BuildCompareModel` is not a rounding error.
static CompareBuildProfile BuildCompareModelProfiledInto(CompareModel& model,
                                                         CompareTextBuffer left_buffer,
                                                         CompareTextBuffer right_buffer,
                                                         const CompareBuildOptions& options) {
  CompareBuildProfile profile;
  // Drop the row views BEFORE the source buffers are replaced: every row views
  // into them, so a row surviving this would read bytes that no longer belong to
  // it. (The row STORAGE is still recycled by RowSink below, which is what keeps a
  // rebuild allocation-free — only the views are cleared here.)
  for (CompareRow& row : model.rows) {
    row.left_text = {};
    row.right_text = {};
  }
  model.hunks.clear();
  // Not a ternary: with EmptyCompareText() as one arm, the other arm's std::move
  // was converted to a const lvalue and copied.
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

  // A non-empty buffer that does not end in '\n' lacks a trailing newline; the
  // patch generator uses this to emit `\ No newline at end of file`.
  model.left_final_newline_missing = !left.empty() && left.back() != '\n';
  model.right_final_newline_missing = !right.empty() && right.back() != '\n';
  model.left_absent = options.left_exists.has_value() ? !*options.left_exists : left.empty();
  model.right_absent = options.right_exists.has_value() ? !*options.right_exists : right.empty();
  // Classify each side's line terminator from its first newline. git keeps the
  // `\r` of a CRLF file inside the blob, so the patch generator must re-emit it.
  const auto uses_crlf = [](const std::string& buffer) {
    const std::size_t newline = buffer.find('\n');
    return newline != std::string::npos && newline > 0 && buffer[newline - 1] == '\r';
  };
  model.left_uses_crlf = uses_crlf(left);
  model.right_uses_crlf = uses_crlf(right);

  const Clock::time_point total_start = Clock::now();

  const Clock::time_point split_start = Clock::now();
  // Views into `left`/`right`, which outlive this build. Everything between here
  // and row assembly stays views; the rows themselves view into the same buffers.
  //
  // Split into the model's retained scratch rather than two fresh vectors: this
  // was the single largest allocator on the keystroke path of an editable
  // compare pane (TD-2026-08-17-261).
  detail::CompareBuildScratch& scratch = EnsureBuildScratch(model);
  util::SplitLineViewsInto(left, scratch.left_lines);
  util::SplitLineViewsInto(right, scratch.right_lines);
  // The element after a final '\n' (and the single element of an empty buffer)
  // is the trailing phantom: not a line, the marker that the file ended in a
  // newline. Keep it out of the alignment so it can pair only with the other
  // side's phantom, appended as a row after the last real line below. Aligned
  // with the real lines, it matched any empty line: a blank line inserted at the
  // end of a file that also lost its final newline came out as an Unchanged row
  // (HEAD's phantom against the new blank line) that belonged to no hunk, so it
  // could be neither staged nor navigated to, and a file emptied to a single
  // blank line diffed as unchanged.
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

  // The all-equal case is handled by the general prefix/suffix path below: when
  // every line matches, `prefix` walks the whole document, `left_middle`/
  // `right_middle` are empty, and the prefix loop emits one Unchanged row per line
  // with left_line == right_line — byte-identical to a dedicated fast path. A prior
  // `lines_equal` pre-scan re-walked all lines via LinesEqualForDiff only to have
  // the prefix loop immediately re-scan the identical prefix; it was pure redundant
  // work per rebuild and has been removed.
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
  // The lexical state each side is in at the next hunk's first line, carried
  // down the file one hunk at a time: a hunk inside a comment or string that
  // opened above it is read as that comment or string (TD-2026-11-11-441).
  struct SideScan {
    DiffLexState state;
    std::size_t line = 0;  // 0-based line `state` holds at
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
      // Under ignore_whitespace an Equal op can pair two whitespace-different
      // lines, so the right column must show the right file's own text.
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
  // The phantom rows (see the split above): one Unchanged row when both sides
  // end in a newline, else a one-sided row that is its own hunk — the change of
  // final-newline state, which the patch generator expresses on the last real
  // line.
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
      // Adjacent to a changed run it joins that hunk: the patch generator states
      // a final-newline change on the last real line, which context expansion
      // cannot reach past a changed row. Otherwise it is a hunk of its own.
      if (!model.hunks.empty() && model.hunks.back().end_row == row_index - 1) {
        model.hunks.back().end_row = row_index;
      } else {
        model.hunks.push_back(CompareHunk{
            .index = static_cast<int>(model.hunks.size()),
            .start_row = row_index,
            .end_row = row_index,
        });
      }
      // The row has to point BACK at that hunk too. Everything that acts on a
      // hunk resolves it from the row under the cursor -- staging, discarding,
      // review markers, hunk navigation, and the context-collapse run that asks
      // its neighbours which hunk they belong to. Left at -1 the row is covered
      // by a hunk that nothing can reach from it, so a file whose only change is
      // its trailing newline had a visible hunk and no working hunk action.
      row.hunk = model.hunks.back().index;
    }
  }
  sink.Finish();
  if (options.refine_hunks) {
    MarkMovedLines(model);  // what the view shows; the patch generator does not ask
  }

  profile.total_ns = DurationNs(total_start, Clock::now());
  // Saturating subtract: the residual row-assembly time is total minus the
  // measured sub-steps, but clock jitter / nested measurement overhead can make
  // the sub-steps sum to slightly more than total. On unsigned fields that would
  // wrap to an enormous value and pollute the perf profile, so floor at zero.
  scratch.hunk_refinements.EndBuild();
  profile.structural_hunks = refinement_stats.structural_hunks;
  profile.cached_hunks = refinement_stats.cached_hunks;
  profile.flat_fallback_hunks = refinement_stats.flat_fallback_hunks;
  profile.graph_vertices = refinement_stats.graph_vertices;
  const std::uint64_t measured_substeps =
      profile.split_lines_ns + profile.line_alignment_ns + profile.hunk_refinement_ns;
  profile.row_assembly_ns = util::SaturatingSub(profile.total_ns, measured_substeps);

  // The stage profile above has always existed, but only microide_diff_bench
  // ever read it -- a compare that felt slow in the running app had no way to
  // say which stage was responsible. Fold the same numbers into the ranked
  // summary so a live session answers that question directly. Off unless
  // MICROIDE_PERF_SUMMARY is set, and the timings are already taken either way.
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

}  // namespace microide::compare
