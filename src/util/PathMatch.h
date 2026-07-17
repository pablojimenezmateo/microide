#pragma once

#include <filesystem>
#include <optional>
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

// Forward-slashed, project-relative representation of `candidate` when it is nested
// under `root`, else nullopt. Purely lexical — never touches the filesystem — so it
// stays correct and cheap on deleted paths, symlinks, or inaccessible mounts, where
// std::filesystem::relative would stat/canonicalize and can fail, slow, or depend on
// the process cwd. Returns nullopt when either input is empty, when candidate is root
// itself, or when candidate escapes root via `..`. Use this for display labels and
// any prefix trimming that must not incur a syscall per path.
[[nodiscard]] inline std::optional<std::string> RelativePathWithin(
    const std::filesystem::path& candidate, const std::filesystem::path& root) {
  if (candidate.empty() || root.empty()) {
    return std::nullopt;
  }
  const std::filesystem::path relative =
      candidate.lexically_normal().lexically_relative(root.lexically_normal());
  if (relative.empty()) {
    return std::nullopt;
  }
  std::string text = relative.generic_string();
  if (text == "." || text == ".." || text.rfind("../", 0) == 0) {
    return std::nullopt;
  }
  return text;
}

// Rewrite `path` so a leading `old_prefix` becomes `new_prefix`, returning the
// normalized result. Paths outside `old_prefix` are returned unchanged (still
// normalized). Purely lexical like PathEqualsOrWithin — no filesystem access, no
// symlink resolution — so it is safe to call once per stored path when remapping
// after a rename without incurring a syscall per entry.
[[nodiscard]] inline std::filesystem::path ReplacePathPrefix(
    const std::filesystem::path& path,
    const std::filesystem::path& old_prefix,
    const std::filesystem::path& new_prefix) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  const std::filesystem::path normalized_old_prefix = old_prefix.lexically_normal();
  const std::filesystem::path normalized_new_prefix = new_prefix.lexically_normal();
  if (!PathEqualsOrWithin(normalized_path, normalized_old_prefix)) {
    return normalized_path;
  }
  if (normalized_path == normalized_old_prefix) {
    return normalized_new_prefix;
  }
  const std::filesystem::path relative =
      normalized_path.lexically_relative(normalized_old_prefix);
  if (relative.empty()) {
    return normalized_path;
  }
  return (normalized_new_prefix / relative).lexically_normal();
}

}  // namespace microide::util
