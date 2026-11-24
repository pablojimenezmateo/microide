#pragma once

#include <utility>

namespace microide::util {

// Call an optional hook (a std::function or function pointer that may be empty)
// when it is set: the shape of every Operations struct's optional callbacks, which
// each call site otherwise guarded by hand. The result, if any, is discarded.
template <typename Callback, typename... Args>
void InvokeIfSet(const Callback& callback, Args&&... args) {
  if (callback) {
    (void)callback(std::forward<Args>(args)...);
  }
}

}  // namespace microide::util
