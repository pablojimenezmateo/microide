#pragma once

#include <cstdint>

namespace microide::util {

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

}
