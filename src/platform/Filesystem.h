#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <system_error>
#include <vector>

namespace microide::platform {

// Upper bound on entries visited by a single recursive tree walk (snapshot or
// watch-path collection). Trees larger than this cannot be watched/diffed
// affordably, so walks stop here and report truncation; the watcher then
// degrades to "too large" mode instead of melting a core. See FileWatcher.cpp.
inline constexpr std::size_t kTreeTraversalEntryBudget = 50000;

// Upper bound on recursion depth for hand-rolled recursive tree walks (project
// file scan, directory-tree expansion). Bounds native-stack use so a
// pathologically deep directory tree cannot overflow the stack. Real projects
// nest far below this.
inline constexpr int kMaxTreeWalkDepth = 512;

enum class PathType {
  Missing,
  RegularFile,
  Directory,
  Other,
};

struct DirectoryEntry {
  std::filesystem::path path;
  PathType type = PathType::Other;

  bool operator==(const DirectoryEntry&) const = default;
};

struct TreeSnapshotEntry {
  std::filesystem::path path;
  PathType type = PathType::Other;
  std::uintmax_t size = 0;
  std::filesystem::file_time_type write_time{};

  bool operator==(const TreeSnapshotEntry&) const = default;
};

using TreeTraversalFilter = std::function<bool(const std::filesystem::path&, PathType)>;

// Classify a std::filesystem::file_status. Split out because both the directory
// walkers in Filesystem.cpp and the watcher's tree scan in FileWatcher.cpp need
// it and each had a byte-identical private copy — and the mapping is a policy
// choice, not a formality: `none` (a status the OS could not determine) is
// deliberately folded in with `not_found` as Missing rather than Other, so a
// stat that fails reads as "gone" to both callers.
inline PathType PathTypeFromStatus(const std::filesystem::file_status& status) {
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

// PathType for an entry a directory iterator just handed back, WITHOUT the
// stat() that `PathTypeFromStatus(entry.status(ec))` costs.
//
// `directory_entry::status()` always calls the free function, which is one
// newfstatat per entry. `is_directory()` / `is_regular_file()` go through
// libstdc++'s `_M_file_type()`, which answers from the type readdir already
// reported in `d_type` and only falls back to a stat when the OS did not report
// one (or reported a symlink, which has to be followed to be classified). On
// this tree that is the difference between 50,844 syscalls and zero — measured
// at 712 ms vs 127 ms for the identical walk and the identical classification.
//
// Every recursive walk in the app funnels through here: the file index's initial
// scan, the inotify registration walk, the poll re-walk, and the sidebar's
// per-directory listing. `ec` is set when the entry could not be classified at
// all (a dangling symlink, a race with deletion); callers skip those.
inline PathType EntryPathType(const std::filesystem::directory_entry& entry,
                              std::error_code& ec) {
  ec.clear();
  if (entry.is_directory(ec)) {
    return PathType::Directory;
  }
  if (ec) {
    return PathType::Missing;
  }
  if (entry.is_regular_file(ec)) {
    return PathType::RegularFile;
  }
  if (ec) {
    return PathType::Missing;
  }
  return PathType::Other;
}

PathType ReadPathType(const std::filesystem::path& path);
std::vector<DirectoryEntry> ListDirectory(const std::filesystem::path& directory);
// Captures size+mtime for every file under `roots` (directories excluded),
// applying `filter`. If `max_entries` is non-zero the walk stops after visiting
// that many directory-iterator entries and sets `*truncated` (when provided);
// `max_entries == 0` means unbounded.
std::vector<TreeSnapshotEntry> CaptureTreeSnapshot(
    const std::vector<std::filesystem::path>& roots,
    const TreeTraversalFilter& filter = {},
    std::size_t max_entries = 0,
    bool* truncated = nullptr);

}  // namespace microide::platform
