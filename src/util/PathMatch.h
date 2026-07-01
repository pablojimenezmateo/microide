#pragma once

#include <filesystem>
#include <string>

namespace microide::util {

// True when two paths denote the same location once normalized. The raw `==`
// comparison runs first so the common case (both already normalized, which is how
// the workspace stores document paths) never allocates the temporaries that
// `lexically_normal()` produces; the normalization only happens on a mismatch.
[[nodiscard]] inline bool SamePathNormalized(const std::filesystem::path& a,
                                             const std::filesystem::path& b) {
  return a == b || a.lexically_normal() == b.lexically_normal();
}

// True when `candidate` is `root` itself or a path nested under it. Purely
// lexical: it never touches the filesystem (no symlink resolution, no cwd
// dependency), which keeps it cheap enough for hot path-prefix scans in the
// diagnostics/decoration stores and deterministic regardless of the process's
// working directory. Empty inputs are never "within" anything.
[[nodiscard]] inline bool PathEqualsOrWithin(const std::filesystem::path& candidate,
                                             const std::filesystem::path& root) {
  if (candidate.empty() || root.empty()) {
    return false;
  }
  if (candidate == root) {
    return true;  // already-normalized identical paths: skip the normalize cost
  }
  const std::filesystem::path normalized_candidate = candidate.lexically_normal();
  const std::filesystem::path normalized_root = root.lexically_normal();
  if (normalized_candidate.empty() || normalized_root.empty()) {
    return false;
  }
  if (normalized_candidate == normalized_root) {
    return true;
  }
  const std::filesystem::path relative = normalized_candidate.lexically_relative(normalized_root);
  if (relative.empty()) {
    return false;
  }
  const std::string relative_text = relative.generic_string();
  return relative_text != "." && relative_text != ".." && relative_text.rfind("../", 0) != 0;
}

}  // namespace microide::util
