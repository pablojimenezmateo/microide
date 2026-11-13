#include "compare/StructuralDiff.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/StructuralSearch.h"

namespace microide::compare {

void StructuralMatch::Reset(std::size_t lhs_tokens, std::size_t rhs_tokens) {
  lhs_partner.assign(lhs_tokens, kNoDiffNode);
  rhs_partner.assign(rhs_tokens, kNoDiffNode);
  lhs_replaced.assign(lhs_tokens, 0);
  rhs_replaced.assign(rhs_tokens, 0);
}

namespace {

using detail::IsLiteral;
using detail::IsPunctuationAtom;
using detail::MarkSubtreesUnchanged;
using detail::SiblingScope;

// The largest sibling-LCS table (cells) the matcher fills to split a run.
constexpr std::size_t kMaxSiblingLcsCells = 1u << 16;

// One sub-problem of a comparison: a run of siblings on each side, each run
// given by its first node and the sibling it stops before.
struct Problem {
  SiblingScope lhs_scope;
  SiblingScope rhs_scope;
  std::uint32_t lhs_first = kNoDiffNode;
  std::uint32_t rhs_first = kNoDiffNode;
};

struct MatcherScratch {
  std::vector<std::uint32_t> lhs_run;
  std::vector<std::uint32_t> rhs_run;
  // Matcher: the work list and the patience-anchoring tables (indexed by
  // content id, reset lazily by stamp).
  std::vector<Problem> problems;
  std::vector<std::uint32_t> id_stamp;
  std::vector<std::uint32_t> id_lhs_count;
  std::vector<std::uint32_t> id_rhs_count;
  std::vector<std::uint32_t> id_lhs_position;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> candidates;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> anchors;
  std::vector<std::uint32_t> lis_tails;
  std::vector<std::uint32_t> lcs_table;
  std::vector<std::uint32_t> lis_previous;
  std::vector<std::string_view> flat_lhs;
  std::vector<std::string_view> flat_rhs;
};

MatcherScratch& Scratch() {
  thread_local MatcherScratch scratch;
  return scratch;
}

void CollectRun(const DiffSyntaxTree& tree, std::uint32_t first, std::uint32_t end,
                std::vector<std::uint32_t>& run) {
  run.clear();
  for (std::uint32_t node = first; node != end && node != kNoDiffNode;
       node = tree.nodes[node].next_sibling) {
    run.push_back(node);
  }
}

// Flat LCS over the token texts of [lhs_begin, lhs_end) and [rhs_begin, rhs_end).
//
// Most calls are small gaps, many per hunk, so a range that fits
// kMaxSiblingLcsCells takes an in-place DP over retained scratch (matched
// bytes maximized, so a word outweighs a separator); only a larger range goes
// to the line differ, whose bounded anchored fallback it needs but whose
// per-call hashing and allocation a small gap does not.
void MatchTokenRangeFlat(const DiffSyntaxTree& lhs, std::uint32_t lhs_begin, std::uint32_t lhs_end,
                         const DiffSyntaxTree& rhs, std::uint32_t rhs_begin, std::uint32_t rhs_end,
                         StructuralMatch& match) {
  MatcherScratch& scratch = Scratch();
  const auto pair = [&](std::uint32_t left, std::uint32_t right) {
    match.lhs_partner[left] = right;
    match.rhs_partner[right] = left;
  };
  const std::size_t rows = lhs_end - lhs_begin;
  const std::size_t columns = rhs_end - rhs_begin;
  if ((rows + 1) * (columns + 1) <= kMaxSiblingLcsCells) {
    const std::size_t stride = columns + 1;
    std::vector<std::uint32_t>& table = scratch.lcs_table;
    table.assign((rows + 1) * stride, 0);
    const auto weight = [&](std::size_t i, std::size_t j) -> std::uint32_t {
      const std::string_view a = lhs.tokens[lhs_begin + i].text;
      return a == rhs.tokens[rhs_begin + j].text ? static_cast<std::uint32_t>(a.size()) : 0;
    };
    for (std::size_t i = rows; i-- > 0;) {
      std::uint32_t* row = table.data() + i * stride;
      const std::uint32_t* below = row + stride;
      for (std::size_t j = columns; j-- > 0;) {
        std::uint32_t best = std::max(below[j], row[j + 1]);
        if (const std::uint32_t w = weight(i, j); w > 0) {
          best = std::max(best, w + below[j + 1]);
        }
        row[j] = best;
      }
    }
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < rows && j < columns) {
      const std::uint32_t w = weight(i, j);
      if (w > 0 && table[i * stride + j] == w + table[(i + 1) * stride + j + 1]) {
        pair(static_cast<std::uint32_t>(lhs_begin + i), static_cast<std::uint32_t>(rhs_begin + j));
        ++i;
        ++j;
      } else if (table[(i + 1) * stride + j] >= table[i * stride + j + 1]) {
        ++i;
      } else {
        ++j;
      }
    }
    return;
  }
  scratch.flat_lhs.clear();
  scratch.flat_rhs.clear();
  for (std::uint32_t index = lhs_begin; index < lhs_end; ++index) {
    scratch.flat_lhs.push_back(lhs.tokens[index].text);
  }
  for (std::uint32_t index = rhs_begin; index < rhs_end; ++index) {
    scratch.flat_rhs.push_back(rhs.tokens[index].text);
  }
  const std::vector<DiffOp> ops = BuildLineDiffOps(scratch.flat_lhs, scratch.flat_rhs);
  std::uint32_t left = lhs_begin;
  std::uint32_t right = rhs_begin;
  for (const DiffOp& op : ops) {
    switch (op.kind) {
      case DiffOpKind::Equal:
        pair(left++, right++);
        break;
      case DiffOpKind::Delete:
        ++left;
        break;
      case DiffOpKind::Insert:
        ++right;
        break;
    }
  }
}

// Splits comparisons into the smallest searches that give the same answer, the
// way difftastic does before its own graph search:
//
//  - equal leading and trailing siblings match outright (no cheaper path can
//    leave an equal pair unmatched at the ends);
//  - when one list is all that is left on each side and the two have the same
//    delimiters, entering both is the cheapest move there is, so the
//    delimiters match and the comparison continues inside -- an edit deep in a
//    large block never builds a graph over the block;
//  - siblings that occur exactly once on each side with equal content anchor
//    the comparison (patience diff over subtrees), and only the gaps between
//    anchors are searched.
//
// What is left goes to the graph search; a search that exceeds the budget falls
// back to the flat token diff for its run only. An explicit work list, not
// recursion: nesting depth is input-controlled.
class Matcher {
 public:
  Matcher(const DiffSyntaxTree& lhs, const DiffSyntaxTree& rhs, std::size_t budget,
          StructuralMatch& match, StructuralDiffStats* stats)
      : lhs_(lhs), rhs_(rhs), budget_(budget), match_(match), stats_(stats), s_(Scratch()) {}

  void Run() {
    std::size_t max_id = 0;
    for (const DiffSyntaxNode& node : lhs_.nodes) {
      max_id = std::max<std::size_t>(max_id, node.content_id);
    }
    for (const DiffSyntaxNode& node : rhs_.nodes) {
      max_id = std::max<std::size_t>(max_id, node.content_id);
    }
    s_.id_stamp.assign(max_id + 1, 0);
    s_.id_lhs_count.assign(max_id + 1, 0);
    s_.id_rhs_count.assign(max_id + 1, 0);
    s_.id_lhs_position.assign(max_id + 1, 0);
    stamp_ = 0;

    s_.problems.clear();
    s_.problems.push_back(Problem{.lhs_first = lhs_.first_root, .rhs_first = rhs_.first_root});
    while (!s_.problems.empty()) {
      const Problem problem = s_.problems.back();
      s_.problems.pop_back();
      Solve(problem);
    }
  }

 private:
  void Solve(const Problem& problem) {
    CollectRun(lhs_, problem.lhs_first, problem.lhs_scope.end, s_.lhs_run);
    CollectRun(rhs_, problem.rhs_first, problem.rhs_scope.end, s_.rhs_run);
    const std::vector<std::uint32_t>& lhs_run = s_.lhs_run;
    const std::vector<std::uint32_t>& rhs_run = s_.rhs_run;

    std::size_t begin = 0;
    while (begin < lhs_run.size() && begin < rhs_run.size() &&
           lhs_.nodes[lhs_run[begin]].content_id == rhs_.nodes[rhs_run[begin]].content_id) {
      MarkSubtreesUnchanged(lhs_, lhs_run[begin], rhs_, rhs_run[begin], match_);
      ++begin;
    }
    std::size_t lhs_end = lhs_run.size();
    std::size_t rhs_end = rhs_run.size();
    while (lhs_end > begin && rhs_end > begin &&
           lhs_.nodes[lhs_run[lhs_end - 1]].content_id ==
               rhs_.nodes[rhs_run[rhs_end - 1]].content_id) {
      --lhs_end;
      --rhs_end;
      MarkSubtreesUnchanged(lhs_, lhs_run[lhs_end], rhs_, rhs_run[rhs_end], match_);
    }
    if (begin == lhs_end || begin == rhs_end) {
      return;  // one side is all novel; nothing to search
    }

    const std::uint32_t lhs_first = lhs_run[begin];
    const std::uint32_t rhs_first = rhs_run[begin];
    const SiblingScope lhs_scope{problem.lhs_scope.parent,
                                 lhs_end < lhs_run.size() ? lhs_run[lhs_end] : problem.lhs_scope.end};
    const SiblingScope rhs_scope{problem.rhs_scope.parent,
                                 rhs_end < rhs_run.size() ? rhs_run[rhs_end] : problem.rhs_scope.end};

    if (lhs_end - begin == 1 && rhs_end - begin == 1) {
      const DiffSyntaxNode& a = lhs_.nodes[lhs_first];
      const DiffSyntaxNode& b = rhs_.nodes[rhs_first];
      if (a.is_list && b.is_list && lhs_.OpenToken(a).text == rhs_.OpenToken(b).text &&
          lhs_.CloseToken(a).text == rhs_.CloseToken(b).text) {
        MatchToken(a.token_begin, b.token_begin);
        MatchToken(a.token_end - 1, b.token_end - 1);
        if (a.first_child != kNoDiffNode && b.first_child != kNoDiffNode) {
          s_.problems.push_back(Problem{.lhs_scope = {lhs_first, kNoDiffNode},
                                        .rhs_scope = {rhs_first, kNoDiffNode},
                                        .lhs_first = a.first_child,
                                        .rhs_first = b.first_child});
        }
        return;
      }
    }

    if (SplitAtUniqueAnchors(begin, lhs_end, begin, rhs_end, lhs_scope, rhs_scope) ||
        SplitAtCommonSubtrees(begin, lhs_end, begin, rhs_end, lhs_scope, rhs_scope)) {
      return;
    }

    const std::uint32_t lhs_token_begin = lhs_.nodes[lhs_first].token_begin;
    const std::uint32_t lhs_token_end = lhs_.nodes[lhs_run[lhs_end - 1]].token_end;
    const std::uint32_t rhs_token_begin = rhs_.nodes[rhs_first].token_begin;
    const std::uint32_t rhs_token_end = rhs_.nodes[rhs_run[rhs_end - 1]].token_end;

    // A gap of bare atoms has no nesting for the graph to reason about: with no
    // list on either side, and no literal on both sides to pair as replaced,
    // the cheapest path is exactly a longest common subsequence of the atoms,
    // which the flat diff finds without a graph.
    if (IsFlat(lhs_, lhs_run, begin, lhs_end, rhs_, rhs_run, begin, rhs_end)) {
      MatchTokenRangeFlat(lhs_, lhs_token_begin, lhs_token_end, rhs_, rhs_token_begin,
                          rhs_token_end, match_);
      return;
    }

    const std::size_t token_product =
        static_cast<std::size_t>(lhs_token_end - lhs_token_begin) *
        static_cast<std::size_t>(rhs_token_end - rhs_token_begin);
    bool found = false;
    if (token_product <= kStructuralSearchMaxTokenProduct) {
      std::size_t vertices = 0;
      found = detail::SearchGap(lhs_, rhs_, lhs_scope, rhs_scope, lhs_first, rhs_first, budget_,
                                match_, vertices);
      if (stats_ != nullptr) {
        stats_->vertices += vertices;
      }
    }
    if (!found) {
      if (stats_ != nullptr) {
        stats_->fell_back = true;
      }
      MatchTokenRangeFlat(lhs_, lhs_token_begin, lhs_token_end, rhs_, rhs_token_begin,
                          rhs_token_end, match_);
    }
  }

  static bool IsFlat(const DiffSyntaxTree& lhs, const std::vector<std::uint32_t>& lhs_run,
                     std::size_t lhs_begin, std::size_t lhs_end, const DiffSyntaxTree& rhs,
                     const std::vector<std::uint32_t>& rhs_run, std::size_t rhs_begin,
                     std::size_t rhs_end) {
    bool lhs_literal = false;
    bool rhs_literal = false;
    for (std::size_t index = lhs_begin; index < lhs_end; ++index) {
      const DiffSyntaxNode& node = lhs.nodes[lhs_run[index]];
      if (node.is_list) {
        return false;
      }
      lhs_literal = lhs_literal || IsLiteral(lhs.OpenToken(node));
    }
    for (std::size_t index = rhs_begin; index < rhs_end; ++index) {
      const DiffSyntaxNode& node = rhs.nodes[rhs_run[index]];
      if (node.is_list) {
        return false;
      }
      rhs_literal = rhs_literal || IsLiteral(rhs.OpenToken(node));
    }
    return !(lhs_literal && rhs_literal);
  }

  // Patience anchoring over the runs' [begin, end) slices. Returns true (and
  // queues the gaps) when at least one anchor was found.
  bool SplitAtUniqueAnchors(std::size_t lhs_begin, std::size_t lhs_end, std::size_t rhs_begin,
                            std::size_t rhs_end, SiblingScope lhs_scope, SiblingScope rhs_scope) {
    const std::vector<std::uint32_t>& lhs_run = s_.lhs_run;
    const std::vector<std::uint32_t>& rhs_run = s_.rhs_run;
    ++stamp_;
    const auto touch = [&](std::uint32_t id) {
      if (s_.id_stamp[id] != stamp_) {
        s_.id_stamp[id] = stamp_;
        s_.id_lhs_count[id] = 0;
        s_.id_rhs_count[id] = 0;
      }
    };
    for (std::size_t index = lhs_begin; index < lhs_end; ++index) {
      const std::uint32_t id = lhs_.nodes[lhs_run[index]].content_id;
      touch(id);
      ++s_.id_lhs_count[id];
      s_.id_lhs_position[id] = static_cast<std::uint32_t>(index);
    }
    for (std::size_t index = rhs_begin; index < rhs_end; ++index) {
      const std::uint32_t id = rhs_.nodes[rhs_run[index]].content_id;
      touch(id);
      ++s_.id_rhs_count[id];
    }
    // Candidates in RHS order, carrying their LHS position; a lone separator
    // is too weak to anchor on.
    s_.candidates.clear();
    for (std::size_t index = rhs_begin; index < rhs_end; ++index) {
      const std::uint32_t node = rhs_run[index];
      const std::uint32_t id = rhs_.nodes[node].content_id;
      if (s_.id_lhs_count[id] == 1 && s_.id_rhs_count[id] == 1 &&
          !IsPunctuationAtom(rhs_, rhs_.nodes[node])) {
        s_.candidates.push_back({s_.id_lhs_position[id], static_cast<std::uint32_t>(index)});
      }
    }
    if (s_.candidates.empty()) {
      return false;
    }
    // Longest chain increasing in LHS position (patience sorting).
    std::vector<std::uint32_t>& tails = s_.lis_tails;
    std::vector<std::uint32_t>& previous = s_.lis_previous;
    tails.clear();
    previous.assign(s_.candidates.size(), kNoDiffNode);
    for (std::uint32_t index = 0; index < s_.candidates.size(); ++index) {
      const auto at = std::lower_bound(
          tails.begin(), tails.end(), s_.candidates[index].first,
          [&](std::uint32_t tail, std::uint32_t value) { return s_.candidates[tail].first < value; });
      if (at != tails.begin()) {
        previous[index] = *(at - 1);
      }
      if (at == tails.end()) {
        tails.push_back(index);
      } else {
        *at = index;
      }
    }
    s_.anchors.clear();
    for (std::uint32_t at = tails.back(); at != kNoDiffNode; at = previous[at]) {
      s_.anchors.push_back(s_.candidates[at]);
    }
    std::reverse(s_.anchors.begin(), s_.anchors.end());

    QueueGapsAroundAnchors(lhs_begin, lhs_end, rhs_begin, rhs_end, lhs_scope, rhs_scope);
    return true;
  }

  // The equal subtrees a weighted longest common subsequence over the two
  // sibling runs keeps, weighted by their token counts (a lone separator weighs
  // nothing, so it never anchors). This is difftastic's split_unchanged: when
  // nothing is unique, equal subtrees still cut the comparison into gaps the
  // search can afford. Bounded by kMaxSiblingLcsCells; larger runs are left to
  // the flat diff by the caller's product check anyway.
  bool SplitAtCommonSubtrees(std::size_t lhs_begin, std::size_t lhs_end, std::size_t rhs_begin,
                             std::size_t rhs_end, SiblingScope lhs_scope, SiblingScope rhs_scope) {
    const std::vector<std::uint32_t>& lhs_run = s_.lhs_run;
    const std::vector<std::uint32_t>& rhs_run = s_.rhs_run;
    const std::size_t rows = lhs_end - lhs_begin;
    const std::size_t columns = rhs_end - rhs_begin;
    const std::size_t stride = columns + 1;
    if ((rows + 1) * stride > kMaxSiblingLcsCells) {
      return false;
    }
    const auto weight = [&](std::size_t lhs_index, std::size_t rhs_index) -> std::uint32_t {
      const DiffSyntaxNode& a = lhs_.nodes[lhs_run[lhs_index]];
      const DiffSyntaxNode& b = rhs_.nodes[rhs_run[rhs_index]];
      if (a.content_id != b.content_id || IsPunctuationAtom(lhs_, a)) {
        return 0;
      }
      return a.token_end - a.token_begin;
    };
    std::vector<std::uint32_t>& table = s_.lcs_table;
    table.assign((rows + 1) * stride, 0);
    for (std::size_t i = rows; i-- > 0;) {
      std::uint32_t* row = table.data() + i * stride;
      const std::uint32_t* below = row + stride;
      for (std::size_t j = columns; j-- > 0;) {
        std::uint32_t best = std::max(below[j], row[j + 1]);
        if (const std::uint32_t w = weight(lhs_begin + i, rhs_begin + j); w > 0) {
          best = std::max(best, w + below[j + 1]);
        }
        row[j] = best;
      }
    }
    if (table[0] == 0) {
      return false;
    }
    s_.anchors.clear();
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < rows && j < columns) {
      const std::uint32_t here = table[i * stride + j];
      const std::uint32_t w = weight(lhs_begin + i, rhs_begin + j);
      if (w > 0 && here == w + table[(i + 1) * stride + j + 1]) {
        s_.anchors.push_back({static_cast<std::uint32_t>(lhs_begin + i),
                              static_cast<std::uint32_t>(rhs_begin + j)});
        ++i;
        ++j;
      } else if (table[(i + 1) * stride + j] >= table[i * stride + j + 1]) {
        ++i;
      } else {
        ++j;
      }
    }
    QueueGapsAroundAnchors(lhs_begin, lhs_end, rhs_begin, rhs_end, lhs_scope, rhs_scope);
    return true;
  }

  // Marks every anchor in `s_.anchors` (pairs of run indices, increasing on
  // both sides) unchanged and queues the gaps before, between and after them.
  void QueueGapsAroundAnchors(std::size_t lhs_begin, std::size_t lhs_end, std::size_t rhs_begin,
                              std::size_t rhs_end, SiblingScope lhs_scope,
                              SiblingScope rhs_scope) {
    const std::vector<std::uint32_t>& lhs_run = s_.lhs_run;
    const std::vector<std::uint32_t>& rhs_run = s_.rhs_run;
    std::size_t lhs_from = lhs_begin;
    std::size_t rhs_from = rhs_begin;
    const auto queue_gap = [&](std::size_t lhs_to, std::size_t rhs_to) {
      if (lhs_from < lhs_to && rhs_from < rhs_to) {
        s_.problems.push_back(Problem{
            .lhs_scope = {lhs_scope.parent, lhs_to < lhs_end ? lhs_run[lhs_to] : lhs_scope.end},
            .rhs_scope = {rhs_scope.parent, rhs_to < rhs_end ? rhs_run[rhs_to] : rhs_scope.end},
            .lhs_first = lhs_run[lhs_from],
            .rhs_first = rhs_run[rhs_from]});
      }
    };
    for (const auto& [lhs_index, rhs_index] : s_.anchors) {
      queue_gap(lhs_index, rhs_index);
      MarkSubtreesUnchanged(lhs_, lhs_run[lhs_index], rhs_, rhs_run[rhs_index], match_);
      lhs_from = lhs_index + 1;
      rhs_from = rhs_index + 1;
    }
    queue_gap(lhs_end, rhs_end);
  }

  void MatchToken(std::uint32_t lhs_token, std::uint32_t rhs_token) {
    match_.lhs_partner[lhs_token] = rhs_token;
    match_.rhs_partner[rhs_token] = lhs_token;
  }

  const DiffSyntaxTree& lhs_;
  const DiffSyntaxTree& rhs_;
  std::size_t budget_;
  StructuralMatch& match_;
  StructuralDiffStats* stats_;
  MatcherScratch& s_;
  std::uint32_t stamp_ = 0;
};

}  // namespace

void MatchTokensFlat(const DiffSyntaxTree& lhs, const DiffSyntaxTree& rhs, StructuralMatch& match) {
  MatchTokenRangeFlat(lhs, 0, static_cast<std::uint32_t>(lhs.tokens.size()), rhs, 0,
                      static_cast<std::uint32_t>(rhs.tokens.size()), match);
}

void MatchSyntaxTrees(const DiffSyntaxTree& lhs,
                      const DiffSyntaxTree& rhs,
                      std::size_t vertex_budget,
                      StructuralMatch& match,
                      StructuralDiffStats* stats) {
  match.Reset(lhs.tokens.size(), rhs.tokens.size());
  Matcher(lhs, rhs, vertex_budget, match, stats).Run();
  FixStructuralSliders(lhs, rhs, match);
}

}  // namespace microide::compare
