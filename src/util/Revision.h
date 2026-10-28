#pragma once

#include <cstdint>

namespace microide::util {

// A change counter that never returns to 0 once bumped, so 0 stays free to mean
// "never seen" in a cache that records the revision it was built at.
class Revision {
 public:
  constexpr Revision() = default;
  constexpr explicit Revision(std::uint64_t start) : value_(start) {}

  constexpr std::uint64_t value() const { return value_; }
  constexpr void Bump() {
    if (++value_ == 0) {
      value_ = 1;
    }
  }
  constexpr void Reset(std::uint64_t value) { value_ = value; }

 private:
  std::uint64_t value_ = 0;
};

}  // namespace microide::util
