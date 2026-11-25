#pragma once

#include <cstddef>
#include <cstdint>

#include "compare/DiffSyntax.h"
#include "compare/StructuralDiff.h"

namespace microide::compare::detail {

struct SiblingScope {
  std::uint32_t parent = kNoDiffNode;
  std::uint32_t end = kNoDiffNode;
};

bool IsPunctuationAtom(const DiffSyntaxTree& tree, const DiffSyntaxNode& node);
bool IsLiteral(const DiffToken& token);
void MarkSubtreesUnchanged(const DiffSyntaxTree& lhs, std::uint32_t lhs_node,
                           const DiffSyntaxTree& rhs, std::uint32_t rhs_node,
                           StructuralMatch& match);

bool SearchGap(const DiffSyntaxTree& lhs, const DiffSyntaxTree& rhs, SiblingScope lhs_scope,
               SiblingScope rhs_scope, std::uint32_t lhs_first, std::uint32_t rhs_first,
               std::size_t budget, StructuralMatch& match, std::size_t& vertices);

}
