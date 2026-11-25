#include "workspace/WorkspacePathUtils.h"

#include <algorithm>
#include <cstddef>
#include <string>

#include "util/PathMatch.h"

namespace microide::workspace {

namespace {

bool IsPathSeparator(char c) {
  return c == '/';
}

}

std::string RelativePathLabel(const std::filesystem::path& root,
                              const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }

  const std::string& path_text = path.native();
  const std::string& root_text = root.native();
  if (!root_text.empty() && root_text != "." && path_text.size() > root_text.size() &&
      !util::PathTextNeedsNormalizing(path_text) &&
      !util::PathTextNeedsNormalizing(root_text) &&
      util::NormalizedPathEqualsOrWithin(path, root)) {
    const std::size_t offset =
        IsPathSeparator(root_text.back()) ? root_text.size() : root_text.size() + 1;
    if (offset < path_text.size()) {
      std::string label = path_text.substr(offset);
      return label;
    }
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

std::string PathLineReference(std::string_view path_label, std::size_t first_line, std::size_t last_line) {
  std::string reference(path_label);
  reference.push_back(':');
  reference += std::to_string(first_line + 1);
  if (last_line > first_line) {
    reference.push_back('-');
    reference += std::to_string(last_line + 1);
  }
  return reference;
}

bool PathEqualsOrWithin(const std::filesystem::path& candidate,
                        const std::filesystem::path& root) {
  return util::PathEqualsOrWithin(candidate, root);
}

}
