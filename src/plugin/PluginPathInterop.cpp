#include "plugin/PluginPathInterop.h"

#include "util/PathContainment.h"

namespace microide::plugin::path_interop {

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
    if (util::PathWithinRoot(normalized, root.lexically_normal())) {
      lexical_ok = true;
      break;
    }
  }
  if (!lexical_ok) {
    return std::nullopt;
  }

  // Tier 2: symlink containment. weakly_canonical resolves symlinks in the existing path
  // prefix even when the leaf does not exist yet, so a not-yet-created write target reached
  // through a symlinked parent directory is canonicalized and rejected here rather than
  // escaping the sandbox. Fails closed: ResolveWithinRoot returns nullopt on escape or any
  // canonicalization error, so containment must be provable for the path to be allowed.
  for (const std::filesystem::path& root : allowed_roots) {
    if (std::optional<std::filesystem::path> contained =
            util::ResolveWithinRoot(normalized, root)) {
      return contained;
    }
  }
  return std::nullopt;
}

}  // namespace microide::plugin::path_interop
