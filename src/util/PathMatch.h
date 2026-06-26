#pragma once

#include <filesystem>

namespace microide::util {

// True when two paths denote the same location once normalized. The raw `==`
// comparison runs first so the common case (both already normalized, which is how
// the workspace stores document paths) never allocates the temporaries that
// `lexically_normal()` produces; the normalization only happens on a mismatch.
[[nodiscard]] inline bool SamePathNormalized(const std::filesystem::path& a,
                                             const std::filesystem::path& b) {
  return a == b || a.lexically_normal() == b.lexically_normal();
}

}  // namespace microide::util
