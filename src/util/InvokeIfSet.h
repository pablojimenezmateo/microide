#pragma once

#include <utility>

namespace microide::util {

template <typename Callback, typename... Args>
void InvokeIfSet(const Callback& callback, Args&&... args) {
  if (callback) {
    (void)callback(std::forward<Args>(args)...);
  }
}

}
