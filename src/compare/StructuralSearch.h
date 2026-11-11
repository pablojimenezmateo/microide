#pragma once

#include <cstddef>
#include <cstdint>

#include "compare/DiffSyntax.h"
#include "compare/StructuralDiff.h"

// Internal to the structural diff: the pieces StructuralDiff.cpp's gap
// splitter shares with the graph search in StructuralSearch.cpp. Not an API.
namespace microide::compare::detail {

// A run of siblings one search works on: the children of `parent`
// (kNoDiffNode for the root level) from some first node up to, not including,
// `end` (kNoDiffNode for "to the last child").
struct SiblingScope {
  std::uint32_t parent = kNoDiffNode;
  std::uint32_t end = kNoDiffNode;
};

// A lone punctuation byte (`,`, `;`, `+`...): too weak to anchor a split on.
bool IsPunctuationAtom(const DiffSyntaxTree& tree, const DiffSyntaxNode& node);
// A string literal or a comment.
bool IsLiteral(const DiffToken& token);
// Matches two content-equal subtrees token for token.
void MarkSubtreesUnchanged(const DiffSyntaxTree& lhs, std::uint32_t lhs_node,
                           const DiffSyntaxTree& rhs, std::uint32_t rhs_node,
                           StructuralMatch& match);

// The A* search over one gap: the runs of siblings starting at `lhs_first` /
// `rhs_first` within their scopes. On success marks the match into `match` and
// returns true; returns false, marking nothing, when it would exceed `budget`
// vertices. `vertices` reports how many it created either way.
bool SearchGap(const DiffSyntaxTree& lhs, const DiffSyntaxTree& rhs, SiblingScope lhs_scope,
               SiblingScope rhs_scope, std::uint32_t lhs_first, std::uint32_t rhs_first,
               std::size_t budget, StructuralMatch& match, std::size_t& vertices);

}  // namespace microide::compare::detail
