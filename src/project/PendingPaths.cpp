#include "project/PendingPaths.h"

#include <algorithm>

namespace microide::project {

PendingPaths::PendingPaths(std::vector<std::string> files) : files_(std::move(files)) {
  std::sort(files_.begin(), files_.end());
  files_.erase(std::unique(files_.begin(), files_.end()), files_.end());
  // Bytewise order keeps everything under one "dir/name/" prefix contiguous, so a
  // directory's repeated child is always the one just appended.
  for (const std::string& file : files_) {
    const std::string_view path(file);
    std::size_t start = 0;
    while (true) {
      const std::size_t slash = path.find('/', start);
      const bool is_directory = slash != std::string_view::npos;
      const std::string_view name = path.substr(start, is_directory ? slash - start : std::string_view::npos);
      if (name.empty()) {
        break;  // "a//b" or a trailing slash: not a path a manifest lists
      }
      const std::string_view directory = path.substr(0, start == 0 ? 0 : start - 1);
      auto it = children_.find(directory);
      if (it == children_.end()) {
        it = children_.emplace(std::string(directory), std::vector<Child>{}).first;
      }
      if (it->second.empty() || it->second.back().name != name) {
        it->second.push_back(Child{.name = name, .is_directory = is_directory});
      }
      if (!is_directory) {
        break;
      }
      start = slash + 1;
    }
  }
}

std::span<const PendingPaths::Child> PendingPaths::ChildrenOf(std::string_view directory) const {
  const auto it = children_.find(directory);
  if (it == children_.end()) {
    return {};
  }
  return it->second;
}

}  // namespace microide::project
