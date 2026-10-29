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
  auto path_it = normal.begin();
  for (const std::filesystem::path& part : from) {
    // A trailing separator normalizes to an empty final component. Skipped on its
    // own: advancing the path past it too dropped the path's next component, so
    // under a root spelled `/repo/`, `/repo/src/main.cpp` mapped to `main.cpp`.
    if (part.empty()) {
      continue;
    }
    if (path_it == normal.end() || *path_it != part) {
      return std::nullopt;
    }
    ++path_it;
  }
  std::filesystem::path rerooted = to;
  for (; path_it != normal.end(); ++path_it) {
    if (!path_it->empty()) {
      rerooted /= *path_it;
    }
  }
  return rerooted;
}

// `/repo/` and `/repo` are one root: normalized, without the empty final
// component a trailing separator leaves (which would also ride along into every
// path re-rooted onto it).
std::filesystem::path NormalRoot(const std::filesystem::path& root) {
  std::filesystem::path normal = root.lexically_normal();
  if (normal.has_relative_path() && !normal.has_filename()) {
    normal = normal.parent_path();
  }
  return normal;
}

}  // namespace

RemotePathMap::RemotePathMap(std::filesystem::path local_root, std::filesystem::path host_root)
    : local_root_(NormalRoot(local_root)), host_root_(NormalRoot(host_root)) {}

std::optional<std::filesystem::path> RemotePathMap::ToHost(
    const std::filesystem::path& local_path) const {
  return Reroot(local_path, local_root_, host_root_);
}

std::optional<std::filesystem::path> RemotePathMap::ToLocal(
    const std::filesystem::path& host_path) const {
  return Reroot(host_path, host_root_, local_root_);
}

}  // namespace microide::project::remote
