#include "platform/Filesystem.h"

#include <algorithm>
#include <chrono>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "util/PathMatch.h"

namespace microide::platform {

namespace {

void SortPaths(auto* entries) {
  std::sort(entries->begin(), entries->end(), [](const auto& lhs, const auto& rhs) {
    // Entries come from directory_iterator, so paths are already normal; native()
    // returns a const reference, avoiding a per-comparison normalize + string alloc.
    return lhs.path.native() < rhs.path.native();
  });
}

}  // namespace

std::optional<FileMetadata> ReadFileMetadata(const std::filesystem::path& path) {
  if (path.empty()) {
    return std::nullopt;
  }
#if defined(_WIN32)
  // No single-call equivalent worth hand-rolling here; the POSIX hosts are the
  // ones whose walks this exists for. Same answers, three calls.
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::status(path, error);
  if (error) {
    return std::nullopt;
  }
  FileMetadata metadata;
  metadata.type = PathTypeFromStatus(status);
  if (metadata.type == PathType::RegularFile) {
    std::error_code size_error;
    metadata.size = std::filesystem::file_size(path, size_error);
    if (size_error) {
      metadata.size = 0;
    }
  }
  std::error_code mtime_error;
  metadata.mtime = std::filesystem::last_write_time(path, mtime_error);
  if (mtime_error) {
    metadata.mtime = {};
  }
  return metadata;
#else
  struct ::stat st {};
  // stat(), not lstat(): std::filesystem::file_size / last_write_time / status
  // all resolve symlinks, and this replaces those at call sites that compare
  // stamps with them.
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
  // file_time_type's epoch is not the system clock's, so go through the
  // documented conversion rather than constructing one from a raw duration.
  // This is the same arithmetic libstdc++ performs inside last_write_time.
  using std::chrono::nanoseconds;
  using std::chrono::seconds;
  const std::chrono::sys_time<nanoseconds> system_time{seconds{st.st_mtim.tv_sec} +
                                                       nanoseconds{st.st_mtim.tv_nsec}};
  metadata.mtime = std::chrono::file_clock::from_sys(system_time);
  return metadata;
#endif
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
      // One stat for both, instead of file_size() + last_write_time() each
      // running their own over every entry of the walk.
      if (const std::optional<FileMetadata> metadata = ReadFileMetadata(path)) {
        write_time = metadata->mtime;
        if (type == PathType::RegularFile) {
          size = metadata->size;
        }
      }
    }
    snapshot.push_back(TreeSnapshotEntry{
        // Both call sites hand in an already-normal path (a normalized root, or
        // a directory-iterator entry); the guard keeps the no-op off the walk.
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

    std::error_code error;
    constexpr auto options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(root, options, error), end;
         !error && it != end; it.increment(error)) {
      if (max_entries != 0 && ++visited > max_entries) {
        budget_exhausted = true;
        break;
      }
      std::error_code status_error;
      const PathType type = EntryPathType(*it, status_error);
      if (status_error) {
        continue;
      }
      // Guarded normalization: see EntryPathType's note. A directory iterator's
      // path is already normal, and the unguarded call was ~12 allocations per
      // entry of every poll re-walk.
      std::filesystem::path path_scratch;
      const std::filesystem::path& path = util::NormalizedPathView(it->path(), path_scratch);
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

  if (budget_exhausted && truncated != nullptr) {
    *truncated = true;
  }
  SortPaths(&snapshot);
  return snapshot;
}

}  // namespace microide::platform
