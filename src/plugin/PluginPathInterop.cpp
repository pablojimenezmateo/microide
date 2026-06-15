#include "plugin/PluginPathInterop.h"

#include <system_error>

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

  // Tier 2: symlink containment. Only runs once the lexical test passed and the target
  // exists, so missing write targets stay allowed and escapes already cost nothing.
  std::error_code exists_error;
  if (!std::filesystem::exists(normalized, exists_error) || exists_error) {
    return normalized;
  }
  std::error_code canon_error;
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(normalized, canon_error);
  if (canon_error) {
    return normalized;  // Lexical test already passed; tolerate transient canonicalization errors.
  }
  for (const std::filesystem::path& root : allowed_roots) {
    std::error_code root_error;
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, root_error);
    if (root_error) {
      continue;
    }
    if (WithinRoot(canonical, canonical_root)) {
      return canonical;
    }
  }
  return std::nullopt;
}

}  // namespace microide::plugin::path_interop
