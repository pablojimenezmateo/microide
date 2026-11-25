#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "compare/DiffSyntax.h"

namespace microide::compare {

struct StructuralMatch {
  std::vector<std::uint32_t> lhs_partner;
  std::vector<std::uint32_t> rhs_partner;
  std::vector<std::uint8_t> lhs_replaced;
  std::vector<std::uint8_t> rhs_replaced;

  void Reset(std::size_t lhs_tokens, std::size_t rhs_tokens);
};

struct StructuralDiffStats {
  std::size_t vertices = 0;
  bool fell_back = false;
};

inline constexpr std::size_t kStructuralSearchMaxTokenProduct = 16'384;

inline constexpr std::size_t kStructuralDiffVertexBudget = 250'000;

void MatchSyntaxTrees(const DiffSyntaxTree& lhs,
                      const DiffSyntaxTree& rhs,
                      std::size_t vertex_budget,
                      StructuralMatch& match,
                      StructuralDiffStats* stats = nullptr);

void FixStructuralSliders(const DiffSyntaxTree& lhs, const DiffSyntaxTree& rhs, StructuralMatch& match);

}
