#include "plugin/PluginPathInterop.h"

#include <system_error>
#include <utility>
#include <vector>

namespace microide::plugin::path_interop {
namespace {

// True when `normalized` is `root` itself notwithstanding, strictly inside `root`. Uses the
// same parent-prefix test the buffer-table builder relies on: lexically_relative yields a
// path beginning with ".." exactly when the target escapes the root.
bool WithinRoot(const std::filesystem::path& normalized, const std::filesystem::path& root) {
  if (root.empty()) {
    return false;
  }
  const std::filesystem::path relative = normalized.lexically_relative(root);
  if (relative.empty()) {
    return false;
  }
  return !(relative.begin() != relative.end() &&
           *relative.begin() == std::filesystem::path(".."));
}

// weakly_canonical(root) is a realpath syscall run for every allowed root on every
// sandboxed plugin filesystem call. It was previously memoized per root in a
// thread_local cache, but that cache was keyed only by the lexical root path and
// never invalidated: if an allowed root is a symlink and its target is replaced
// mid-session, containment kept comparing against the stale canonical target
// (denying the new root or, worse, still trusting the old physical directory).
// For a security check that is unacceptable, and a plugin fs call already performs
// real file I/O, so one fresh realpath per allowed root is negligible. Resolve
// live every time. `ok` is false when canonicalization failed.
struct CanonicalRoot {
  std::filesystem::path path;
  bool ok = false;
};

CanonicalRoot CanonicalRootResolved(const std::filesystem::path& root) {
  std::error_code error;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(root, error);
  return CanonicalRoot{.path = std::move(canonical), .ok = !error};
}

}  // namespace

std::string Basename(const std::filesystem::path& path) {
  return path.filename().empty() ? path.lexically_normal().string() : path.filename().string();
}

std::filesystem::path ResolveRuntimePath(const std::filesystem::path& project_root,
                                         const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }
  if (path.is_absolute() || project_root.empty()) {
    return path.lexically_normal();
  }
  return (project_root / path).lexically_normal();
}

std::optional<std::filesystem::path> ContainPath(
    std::span<const std::filesystem::path> allowed_roots,
    const std::filesystem::path& requested) {
  if (requested.empty()) {
    return std::nullopt;
  }
  const std::filesystem::path normalized = requested.lexically_normal();

  // Tier 1: lexical containment. Rejects every `..` escape without touching the filesystem.
  bool lexical_ok = false;
  for (const std::filesystem::path& root : allowed_roots) {
    if (WithinRoot(normalized, root.lexically_normal())) {
      lexical_ok = true;
      break;
    }
  }
  if (!lexical_ok) {
    return std::nullopt;
  }

  // Tier 2: symlink containment. std::filesystem::weakly_canonical resolves symlinks
  // in the EXISTING prefix of the path and appends any non-existent leaf lexically,
  // so it must run even for a MISSING target: creating `link/new.txt`, where the
  // existing parent `link` is a symlink pointing outside the project, would otherwise
  // pass the lexical test (no `..`) and then be written beside the followed symlink
  // target — a containment escape. Canonicalizing the deepest existing parent closes
  // that hole.
  std::error_code canon_error;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(normalized, canon_error);
  if (canon_error) {
    // Retry once: a truly transient error (e.g. a momentary EAGAIN) should not
    // deny a legitimate plugin path. But if canonicalization still fails we must
    // FAIL CLOSED — returning the lexical path here would downgrade the symlink
    // containment check to lexical-only, letting a symlinked prefix whose
    // canonicalization errors (permission error, symlink loop) escape the root.
    canon_error.clear();
    canonical = std::filesystem::weakly_canonical(normalized, canon_error);
    if (canon_error) {
      return std::nullopt;
    }
  }
  for (const std::filesystem::path& root : allowed_roots) {
    const CanonicalRoot canonical_root = CanonicalRootResolved(root);
    if (!canonical_root.ok) {
      continue;
    }
    if (WithinRoot(canonical, canonical_root.path)) {
      return canonical;
    }
  }
  return std::nullopt;
}

}  // namespace microide::plugin::path_interop
