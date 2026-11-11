#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "compare/DiffSyntax.h"

// The structural diff: which tokens of one syntax forest are the "same" tokens
// as tokens of the other, and which are NOVEL.
//
// This is difftastic's formulation. The two forests are walked together as a
// shortest-path problem over vertices (next unmatched LHS node, next unmatched
// RHS node, the delimiters entered so far). Each edge consumes syntax: an equal
// subtree on both sides (cheap), a delimiter pair entered on both sides, a
// string/comment replaced by a similar one, or one node that exists on only one
// side (expensive). Dijkstra finds the cheapest walk, and the novel nodes on it
// are the change. Because equality is structural, re-indenting, re-wrapping or
// re-spacing code costs nothing, and wrapping code in a new block costs only the
// new delimiters and whatever precedes them.
namespace microide::compare {

// The outcome per TOKEN of each tree (DiffSyntaxTree::tokens). A token with a
// partner is unchanged — or, when `replaced` is set, a string or comment whose
// text changed while staying recognisably the same literal; its words still
// need diffing. A token without a partner is novel.
struct StructuralMatch {
  std::vector<std::uint32_t> lhs_partner;
  std::vector<std::uint32_t> rhs_partner;
  std::vector<std::uint8_t> lhs_replaced;
  std::vector<std::uint8_t> rhs_replaced;

  void Reset(std::size_t lhs_tokens, std::size_t rhs_tokens);
};

struct StructuralDiffStats {
  std::size_t vertices = 0;
  // Some gap was too large to search, or its search ran out of budget, so the
  // flat token diff decided that gap.
  bool fell_back = false;
};

// The largest token product (LHS tokens x RHS tokens) one graph search is
// started on. The comparison is first split into independent gaps (equal ends
// trimmed, matching lists entered, unique subtrees anchored), so this bounds
// each GAP, not the hunk or the file; a gap above it -- a large rewrite, where
// the structural reading adds little over a token LCS -- is matched by the flat
// token diff straight away rather than after a search that would exhaust the
// budget below. A gap this size takes about a millisecond to search.
inline constexpr std::size_t kStructuralSearchMaxTokenProduct = 16'384;

// The most vertices one search may create: the safety cap on the A* search for a
// gap that passed the product check above. A search that reaches it falls back
// to the flat token diff for its gap.
inline constexpr std::size_t kStructuralDiffVertexBudget = 250'000;

// Matches `lhs` against `rhs` (both interned by one DiffContentInterner). A gap
// too large to search (kStructuralSearchMaxTokenProduct), or whose search
// exceeds `vertex_budget`, is matched by a flat longest-common-subsequence over
// its token texts instead — still an exact diff (every token it calls unchanged
// IS unchanged), just blind to nesting.
void MatchSyntaxTrees(const DiffSyntaxTree& lhs,
                      const DiffSyntaxTree& rhs,
                      std::size_t vertex_budget,
                      StructuralMatch& match,
                      StructuralDiffStats* stats = nullptr);

// The flat fallback on its own: an LCS over the two token sequences' texts.
void MatchTokensFlat(const DiffSyntaxTree& lhs, const DiffSyntaxTree& rhs, StructuralMatch& match);

}  // namespace microide::compare
