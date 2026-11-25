#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <system_error>
#include <vector>

namespace microide::platform {

inline constexpr std::size_t kTreeTraversalEntryBudget = 50000;

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

struct FileMetadata {
  PathType type = PathType::Missing;
  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
};

[[nodiscard]] std::optional<FileMetadata> ReadFileMetadata(const std::filesystem::path& path);

PathType ReadPathType(const std::filesystem::path& path);
std::vector<DirectoryEntry> ListDirectory(const std::filesystem::path& directory);
std::vector<TreeSnapshotEntry> CaptureTreeSnapshot(
    const std::vector<std::filesystem::path>& roots,
    const TreeTraversalFilter& filter = {},
    std::size_t max_entries = 0,
    bool* truncated = nullptr);

}
