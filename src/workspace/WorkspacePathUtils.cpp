#include "workspace/WorkspacePathUtils.h"

#include <algorithm>
#include <cstddef>
#include <string>

#include "util/PathMatch.h"

namespace microide::workspace {

namespace {

bool IsPathSeparator(char c) {
#ifdef _WIN32
  return c == '\\' || c == '/';
#else
  return c == '/';
#endif
}

}  // namespace

std::string RelativePathLabel(const std::filesystem::path& root,
                              const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }

  // Fast path: both spellings are already lexically normal — which is how the
  // workspace stores every path it holds — and `path` sits under `root`. The
  // label is then a substring of the path's own text, while the general form
  // below spends ~30 allocations arriving at that same substring (two
  // `lexically_normal`s, a `lexically_relative`, a third `lexically_normal`, a
  // `generic_string`, each materialising a component list). This runs per tab on
  // every tab-strip geometry rebuild, which is what made it worth a fast path
  // (TD-2026-08-06-159).
  //
  // A "." root is excluded deliberately: its members are spelled with no "./"
  // prefix once normalized, so there is no prefix to trim and the substring math
  // below would cut into the filename.
  const std::string& path_text = path.native();
  const std::string& root_text = root.native();
  if (!root_text.empty() && root_text != "." && path_text.size() > root_text.size() &&
      !util::PathTextNeedsNormalizing(path_text) &&
      !util::PathTextNeedsNormalizing(root_text) &&
      util::NormalizedPathEqualsOrWithin(path, root)) {
    // Containment held with different lengths, so either the root already ends in
    // a separator or the path carries one right after it.
    const std::size_t offset =
        IsPathSeparator(root_text.back()) ? root_text.size() : root_text.size() + 1;
    if (offset < path_text.size()) {
      std::string label = path_text.substr(offset);
#ifdef _WIN32
      std::replace(label.begin(), label.end(), '\\', '/');
#endif
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

bool PathEqualsOrWithin(const std::filesystem::path& candidate,
                        const std::filesystem::path& root) {
  return util::PathEqualsOrWithin(candidate, root);
}

}  // namespace microide::workspace
