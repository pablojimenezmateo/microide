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

// weakly_canonical(root) is a realpath syscall, and ContainPath runs it for every
// allowed root on every sandboxed plugin filesystem call — but the roots (project
// root, plugin data dir) are stable for a session. Memoize per root in a small
// thread_local cache (plugin fs calls are single-threaded on the worker). `ok` is
// false when canonicalization failed, so the caller skips that root exactly as
// before. A bounded size keeps a pathological caller from growing it unboundedly.
struct CanonicalRoot {
  std::filesystem::path path;
  bool ok = false;
};

const CanonicalRoot& CanonicalRootCached(const std::filesystem::path& root) {
  thread_local std::vector<std::pair<std::filesystem::path, CanonicalRoot>> cache;
  for (const auto& [key, value] : cache) {
    if (key == root) {
      return value;
    }
  }
  std::error_code error;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(root, error);
  CanonicalRoot resolved{.path = std::move(canonical), .ok = !error};
  if (cache.size() >= 16) {
    cache.clear();
  }
  cache.emplace_back(root, std::move(resolved));
  return cache.back().second;
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
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(normalized, canon_error);
  if (canon_error) {
    return normalized;  // Lexical test already passed; tolerate transient canonicalization errors.
  }
  for (const std::filesystem::path& root : allowed_roots) {
    const CanonicalRoot& canonical_root = CanonicalRootCached(root);
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
