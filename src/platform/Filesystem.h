#pragma once

#include <filesystem>
#include <vector>

namespace microide::platform {

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

PathType ReadPathType(const std::filesystem::path& path);
std::vector<DirectoryEntry> ListDirectory(const std::filesystem::path& directory);
std::vector<TreeSnapshotEntry> CaptureTreeSnapshot(const std::vector<std::filesystem::path>& roots);

}  // namespace microide::platform
