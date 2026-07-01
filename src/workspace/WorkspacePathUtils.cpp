#include "workspace/WorkspacePathUtils.h"

#include "util/PathMatch.h"

namespace microide::workspace {

std::string RelativePathLabel(const std::filesystem::path& root,
                              const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }

  const auto normalized_path = path.lexically_normal();
  const auto normalized_root = root.lexically_normal();
  const auto relative = normalized_path.lexically_relative(normalized_root);
  const bool starts_with_parent =
      relative.begin() != relative.end() &&
      *relative.begin() == std::filesystem::path("..");
  if (!relative.empty() && !starts_with_parent) {
    return relative.lexically_normal().generic_string();
  }
  return normalized_path.generic_string();
}

bool PathEqualsOrWithin(const std::filesystem::path& candidate,
                        const std::filesystem::path& root) {
  return util::PathEqualsOrWithin(candidate, root);
}

std::filesystem::path ReplacePathPrefix(const std::filesystem::path& path,
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

}  // namespace microide::workspace
