#include "project/remote/RemotePathMap.h"

#include <utility>

namespace microide::project::remote {
namespace {

// `path` re-rooted from `from` onto `to`, component-wise; nullopt when `path` is
// not under `from`.
std::optional<std::filesystem::path> Reroot(const std::filesystem::path& path,
                                            const std::filesystem::path& from,
                                            const std::filesystem::path& to) {
  const std::filesystem::path normal = path.lexically_normal();
  auto from_it = from.begin();
  auto path_it = normal.begin();
  for (; from_it != from.end(); ++from_it, ++path_it) {
    // A trailing separator normalizes to an empty final component.
    if (from_it->empty()) {
      continue;
    }
    if (path_it == normal.end() || *path_it != *from_it) {
      return std::nullopt;
    }
  }
  std::filesystem::path rerooted = to;
  for (; path_it != normal.end(); ++path_it) {
    if (!path_it->empty()) {
      rerooted /= *path_it;
    }
  }
  return rerooted;
}

}  // namespace

RemotePathMap::RemotePathMap(std::filesystem::path local_root, std::filesystem::path host_root)
    : local_root_(local_root.lexically_normal()), host_root_(host_root.lexically_normal()) {}

std::optional<std::filesystem::path> RemotePathMap::ToHost(
    const std::filesystem::path& local_path) const {
  return Reroot(local_path, local_root_, host_root_);
}

std::optional<std::filesystem::path> RemotePathMap::ToLocal(
    const std::filesystem::path& host_path) const {
  return Reroot(host_path, host_root_, local_root_);
}

}  // namespace microide::project::remote
