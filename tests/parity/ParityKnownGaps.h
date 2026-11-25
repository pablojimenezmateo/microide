#pragma once

#include <array>
#include <string_view>

namespace microide::tests::parity {

struct KnownGap {
  std::string_view scenario;
  std::string_view locality;
  std::string_view removed_by;
};

inline constexpr std::array<KnownGap, 0> kParityKnownGaps{};

}
