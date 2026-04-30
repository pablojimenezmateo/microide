#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

std::string RelativePathLabel(const std::filesystem::path& root,
                              const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }

  std::error_code error;
  const auto relative = std::filesystem::relative(path, root, error);
  const bool starts_with_parent =
      relative.begin() != relative.end() &&
      *relative.begin() == std::filesystem::path("..");
  if (!error && !relative.empty() && !starts_with_parent) {
    return relative.lexically_normal().string();
  }
  return path.lexically_normal().string();
}

bool PathEqualsOrWithin(const std::filesystem::path& candidate,
                        const std::filesystem::path& root) {
  const std::filesystem::path normalized_candidate = candidate.lexically_normal();
  const std::filesystem::path normalized_root = root.lexically_normal();
  if (normalized_candidate.empty() || normalized_root.empty()) {
    return false;
  }
  if (normalized_candidate == normalized_root) {
    return true;
  }

  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(normalized_candidate, normalized_root, error);
  if (error || relative.empty()) {
    return false;
  }
  const std::string relative_text = relative.generic_string();
  return relative_text != "." && relative_text != ".." && relative_text.rfind("../", 0) != 0;
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

  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(normalized_path, normalized_old_prefix, error);
  if (error || relative.empty()) {
    return normalized_path;
  }
  return (normalized_new_prefix / relative).lexically_normal();
}

}  // namespace microide::workspace
