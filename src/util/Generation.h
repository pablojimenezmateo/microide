#pragma once

#include <atomic>
#include <cstdint>

namespace microide::util {

class Generation {
 public:
  using Token = std::uint64_t;

  Token current() const { return value_.load(std::memory_order_acquire); }

  Token bump() { return value_.fetch_add(1, std::memory_order_acq_rel) + 1; }

  bool is_current(Token token) const { return token == current(); }

 private:
  std::atomic<Token> value_{0};
};

}
