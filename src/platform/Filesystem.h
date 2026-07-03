#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
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
