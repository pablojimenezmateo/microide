#pragma once

#include <atomic>
#include <cstdint>

namespace microide::util {

// A monotonic epoch counter for staleness detection. An async operation captures
// `current()` before it starts; when it completes it applies its result only if
// `is_current(captured)` still holds. Any intervening `bump()` (a frame switch, a
// resume, a newer request) invalidates the captured token so the late completion
// is dropped.
//
// Use this for pure-integer epoch guards owned by a service or coordinator. It is
// intentionally NOT for "does this still describe what's on screen" guards that
// re-match concrete state (a path, a view id) — those carry information an integer
// cannot. Being atomic, it is also not for fields embedded in copyable value-state
// structs (it would make the whole struct non-copyable); keep a plain integer there.
class Generation {
 public:
  using Token = std::uint64_t;

  Token current() const { return value_.load(std::memory_order_acquire); }

  // Advance to a fresh epoch and return it. Tokens captured before this call no
  // longer satisfy is_current().
  Token bump() { return value_.fetch_add(1, std::memory_order_acq_rel) + 1; }

  bool is_current(Token token) const { return token == current(); }

 private:
  std::atomic<Token> value_{0};
};

}  // namespace microide::util
