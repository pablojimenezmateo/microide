#pragma once

#include <filesystem>
#include <optional>
#include <system_error>

namespace microide::util {

// Lexical containment test: true when `candidate` resides strictly within `root`
// (and is not `root` itself). Neither path is canonicalized, so this only rejects
// `..` escapes. Callers that must also defeat symlink escapes use ResolveWithinRoot.
// An empty root never contains anything.
inline bool PathWithinRoot(const std::filesystem::path& candidate,
                           const std::filesystem::path& root) {
  if (root.empty()) {
    return false;
  }
  const std::filesystem::path relative = candidate.lexically_relative(root);
  if (relative.empty()) {
    return false;
  }
  return !(relative.begin() != relative.end() &&
           *relative.begin() == std::filesystem::path(".."));
}

// Symlink-safe containment. Resolves symlinks in the existing prefix of `requested`
// (weakly_canonical handles not-yet-created leaves by canonicalizing the longest
// existing prefix and lexically appending the remainder) and confirms the result
// stays within `root` after both are canonicalized. Returns the canonical path on
// success. Fails closed: returns nullopt on escape OR when containment cannot be
// proven (any canonicalization error), so a symlinked parent directory pointing
// outside the root can never redirect a write past the sandbox.
inline std::optional<std::filesystem::path> ResolveWithinRoot(
    const std::filesystem::path& requested, const std::filesystem::path& root) {
  if (requested.empty() || root.empty()) {
    return std::nullopt;
  }
  std::error_code canon_error;
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(requested, canon_error);
  if (canon_error) {
    return std::nullopt;
  }
  std::error_code root_error;
  const std::filesystem::path canonical_root =
      std::filesystem::weakly_canonical(root, root_error);
  if (root_error) {
    return std::nullopt;
  }
  if (PathWithinRoot(canonical, canonical_root)) {
    return canonical;
  }
  return std::nullopt;
}

}  // namespace microide::util
