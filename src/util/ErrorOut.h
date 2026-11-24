#pragma once

#include <string>
#include <utility>

namespace microide::util {

// The optional `std::string* error` out-parameter of a `bool Do(..., std::string* error)`
// API. Every such function wrote the same null-checked assignment at each failure and
// the same null-checked clear on success; these say it once.

// Record `message` when the caller asked for it.
inline void SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

// Record `message` and report failure: `return util::Fail(error, "why");`.
[[nodiscard]] inline bool Fail(std::string* error, std::string message) {
  SetError(error, std::move(message));
  return false;
}

// Forget a previous failure's message.
inline void ClearError(std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
}

}  // namespace microide::util
