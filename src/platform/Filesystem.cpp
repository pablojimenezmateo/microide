#include "platform/Filesystem.h"

#include <algorithm>
#include <system_error>

namespace microide::platform {

namespace {

PathType PathTypeFromStatus(const std::filesystem::file_status& status) {
  switch (status.type()) {
    case std::filesystem::file_type::none:
    case std::filesystem::file_type::not_found:
      return PathType::Missing;
    case std::filesystem::file_type::regular:
      return PathType::RegularFile;
    case std::filesystem::file_type::directory:
      return PathType::Directory;
    default:
      return PathType::Other;
  }
}

void SortPaths(auto* entries) {
  std::sort(entries->begin(), entries->end(), [](const auto& lhs, const auto& rhs) {
    return lhs.path.lexically_normal().generic_string() < rhs.path.lexically_normal().generic_string();
  });
}

}  // namespace

PathType ReadPathType(const std::filesystem::path& path) {
  if (path.empty()) {
    return PathType::Missing;
  }

  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::status(path.lexically_normal(), error);
  return error ? PathType::Missing : PathTypeFromStatus(status);
}

std::vector<DirectoryEntry> ListDirectory(const std::filesystem::path& directory) {
  std::vector<DirectoryEntry> entries;
  if (ReadPathType(directory) != PathType::Directory) {
    return entries;
  }

  std::error_code error;
  for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end;
       it.increment(error)) {
    std::error_code status_error;
    entries.push_back(DirectoryEntry{
        .path = it->path().lexically_normal(),
        .type = PathTypeFromStatus(it->status(status_error)),
    });
  }
  SortPaths(&entries);
  return entries;
}

std::vector<TreeSnapshotEntry> CaptureTreeSnapshot(const std::vector<std::filesystem::path>& roots,
                                                   const TreeTraversalFilter& filter) {
  std::vector<TreeSnapshotEntry> snapshot;
  std::vector<std::filesystem::path> normalized_roots;
  normalized_roots.reserve(roots.size());
  for (const auto& root : roots) {
    if (!root.empty()) {
      normalized_roots.push_back(root.lexically_normal());
    }
  }
  std::sort(normalized_roots.begin(), normalized_roots.end());
  normalized_roots.erase(std::unique(normalized_roots.begin(), normalized_roots.end()),
                         normalized_roots.end());

  const auto append_entry = [&](const std::filesystem::path& path, PathType type) {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type write_time{};
    if (type != PathType::Missing) {
      std::error_code metadata_error;
      if (type == PathType::RegularFile) {
        size = std::filesystem::file_size(path, metadata_error);
        if (metadata_error) {
          size = 0;
        }
      }
      metadata_error.clear();
      write_time = std::filesystem::last_write_time(path, metadata_error);
      if (metadata_error) {
        write_time = std::filesystem::file_time_type{};
      }
    }
    snapshot.push_back(TreeSnapshotEntry{
        .path = path.lexically_normal(),
        .type = type,
        .size = size,
        .write_time = write_time,
    });
  };

  for (const auto& root : normalized_roots) {
    const PathType root_type = ReadPathType(root);
    if (root_type == PathType::Missing) {
      continue;
    }
    if (root_type != PathType::Directory) {
      append_entry(root, root_type);
      continue;
    }

    std::error_code error;
    constexpr auto options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(root, options, error), end;
         !error && it != end; it.increment(error)) {
      const std::filesystem::path path = it->path().lexically_normal();
      std::error_code status_error;
      const PathType type = PathTypeFromStatus(it->status(status_error));
      if (status_error) {
        continue;
      }
      if (filter && !filter(path, type)) {
        if (type == PathType::Directory) {
          it.disable_recursion_pending();
        }
        continue;
      }
      if (type == PathType::Directory) {
        continue;
      }
      append_entry(path, type);
    }
  }

  SortPaths(&snapshot);
  return snapshot;
}

}  // namespace microide::platform
