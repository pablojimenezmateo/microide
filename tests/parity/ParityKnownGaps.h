#pragma once

#include <array>
#include <string_view>

namespace microide::tests::parity {

// Parity rows that cannot pass yet, each with what removes it. The runner fails on
// a listed row that now passes, so this list only shrinks. Phase 2b ships with it
// EMPTY (dev-docs/design/remote-projects.md § 10.1).
struct KnownGap {
  std::string_view scenario;
  std::string_view removed_by;
};

// Empty: every row matches. To add one, bump the size and say what removes it,
// e.g. {"Parity/Scenario", "TD-…"}.
inline constexpr std::array<KnownGap, 0> kParityKnownGaps{};

}  // namespace microide::tests::parity
