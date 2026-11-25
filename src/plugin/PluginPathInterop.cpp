#include "plugin/PluginPathInterop.h"

#include <system_error>
#include <utility>
#include <vector>

namespace microide::plugin::path_interop {
namespace {

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

struct CanonicalRoot {
  std::filesystem::path path;
  bool ok = false;
};

CanonicalRoot CanonicalRootResolved(const std::filesystem::path& root) {
  std::error_code error;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(root, error);
  return CanonicalRoot{.path = std::move(canonical), .ok = !error};
}

}

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

  std::error_code canon_error;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(normalized, canon_error);
  if (canon_error) {
    canon_error.clear();
    canonical = std::filesystem::weakly_canonical(normalized, canon_error);
    if (canon_error) {
      return std::nullopt;
    }
  }
  constexpr int kMaxDanglingSymlinkHops = 16;
  std::filesystem::path resolved = canonical;
  bool hops_exhausted = true;
  for (int hop = 0; hop <= kMaxDanglingSymlinkHops; ++hop) {
    std::error_code link_error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(resolved, link_error);
    if (link_error || !std::filesystem::is_symlink(status)) {
      hops_exhausted = false;
      break;
    }
    if (hop == kMaxDanglingSymlinkHops) {
      break;
    }
    std::filesystem::path target = std::filesystem::read_symlink(resolved, link_error);
    if (link_error) {
      return std::nullopt;
    }
    if (target.is_relative()) {
      target = resolved.parent_path() / target;
    }
    std::error_code hop_error;
    resolved = std::filesystem::weakly_canonical(target, hop_error);
    if (hop_error) {
      return std::nullopt;
    }
  }
  if (hops_exhausted) {
    return std::nullopt;
  }

  for (const std::filesystem::path& root : allowed_roots) {
    const CanonicalRoot canonical_root = CanonicalRootResolved(root);
    if (!canonical_root.ok) {
      continue;
    }
    if (WithinRoot(resolved, canonical_root.path)) {
      return canonical;
    }
  }
  return std::nullopt;
}

}
