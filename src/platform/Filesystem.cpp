#include "platform/Filesystem.h"

#include <algorithm>
#include <chrono>
#include <system_error>

#include <sys/stat.h>

#include "platform/DirectoryWalk.h"
#include "util/PathMatch.h"

namespace microide::platform {

namespace {

void SortPaths(auto* entries) {
  std::sort(entries->begin(), entries->end(), [](const auto& lhs, const auto& rhs) {
    return lhs.path.native() < rhs.path.native();
  });
}

}

std::optional<FileMetadata> ReadFileMetadata(const std::filesystem::path& path) {
  if (path.empty()) {
    return std::nullopt;
  }
  struct ::stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    return std::nullopt;
  }
  FileMetadata metadata;
  if (S_ISREG(st.st_mode)) {
    metadata.type = PathType::RegularFile;
    metadata.size = static_cast<std::uintmax_t>(st.st_size);
  } else if (S_ISDIR(st.st_mode)) {
    metadata.type = PathType::Directory;
  } else {
    metadata.type = PathType::Other;
  }
  using std::chrono::nanoseconds;
  using std::chrono::seconds;
  const std::chrono::sys_time<nanoseconds> system_time{seconds{st.st_mtim.tv_sec} +
                                                       nanoseconds{st.st_mtim.tv_nsec}};
  metadata.mtime = std::chrono::file_clock::from_sys(system_time);
  return metadata;
}

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
        .path = util::NormalizedPath(it->path()),
        .type = EntryPathType(*it, status_error),
    });
  }
  SortPaths(&entries);
  return entries;
}

std::vector<TreeSnapshotEntry> CaptureTreeSnapshot(const std::vector<std::filesystem::path>& roots,
                                                   const TreeTraversalFilter& filter,
                                                   std::size_t max_entries, bool* truncated) {
  if (truncated != nullptr) {
    *truncated = false;
  }
  std::vector<TreeSnapshotEntry> snapshot;
  std::size_t visited = 0;
  bool budget_exhausted = false;
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
      if (const std::optional<FileMetadata> metadata = ReadFileMetadata(path)) {
        write_time = metadata->mtime;
        if (type == PathType::RegularFile) {
          size = metadata->size;
        }
      }
    }
    snapshot.push_back(TreeSnapshotEntry{
        .path = util::NormalizedPath(path),
        .type = type,
        .size = size,
        .write_time = write_time,
    });
  };

  for (const auto& root : normalized_roots) {
    if (budget_exhausted) {
      break;
    }
    const PathType root_type = ReadPathType(root);
    if (root_type == PathType::Missing) {
      continue;
    }
    if (root_type != PathType::Directory) {
      append_entry(root, root_type);
      continue;
    }

    (void) WalkDirectoryTree(root, WalkOptions{}, [&](const WalkEntry& entry) {
      if (max_entries != 0 && ++visited > max_entries) {
        budget_exhausted = true;
        return WalkAction::Stop;
      }
      if (entry.type == PathType::Missing) {
        return WalkAction::Continue;
      }
      const std::filesystem::path path(entry.path);
      if (filter && !filter(path, entry.type)) {
        return WalkAction::Prune;
      }
      if (entry.type != PathType::Directory) {
        append_entry(path, entry.type);
      }
      return WalkAction::Continue;
    });
  }

  if (budget_exhausted && truncated != nullptr) {
    *truncated = true;
  }
  SortPaths(&snapshot);
  return snapshot;
}

}
