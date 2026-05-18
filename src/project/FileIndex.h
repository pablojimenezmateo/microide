#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <vector>

#include "platform/FileIndexWatcher.h"
#include "project/ProjectFileScanner.h"

namespace microide::project {

struct ProjectFile {
  std::filesystem::path relative_path;
  std::filesystem::file_time_type mtime{};
  std::uintmax_t size = 0;

  bool operator==(const ProjectFile&) const = default;
};

struct FileIndexSnapshot {
  std::uint64_t version = 0;
  std::vector<ProjectFile> files;
};

struct FilePathSnapshot {
  std::uint64_t version = 0;
  std::vector<std::filesystem::path> files;
};

class FileIndex {
 public:
  enum class RootPopulationMode {
    ScanNow,
    Deferred,
  };

  FileIndex() = default;
  ~FileIndex() = default;
  FileIndex(FileIndex&& other) noexcept;
  FileIndex& operator=(FileIndex&& other) noexcept;
  FileIndex(const FileIndex&) = delete;
  FileIndex& operator=(const FileIndex&) = delete;

  bool SetRoot(const std::filesystem::path& root,
               RootPopulationMode population_mode = RootPopulationMode::ScanNow);
  void Reset();
  void Refresh();
  bool ApplyBatch(const platform::IndexUpdateBatch& batch);
  std::vector<ProjectFile> Snapshot() const;
  FileIndexSnapshot SnapshotWithVersion() const;
  FilePathSnapshot SnapshotPathsWithVersion(
      ProjectFileScanMode mode = ProjectFileScanMode::ExcludeHidden) const;
  std::vector<std::filesystem::path> SnapshotPaths(
      ProjectFileScanMode mode = ProjectFileScanMode::ExcludeHidden) const;
  const std::vector<std::filesystem::path>& files(
      ProjectFileScanMode mode = ProjectFileScanMode::ExcludeHidden) const;
  std::uint64_t version() const;

  const std::filesystem::path& root() const { return root_; }

 private:
  struct CacheBucket {
    std::vector<std::filesystem::path> files;
    bool needs_refresh = true;
  };

  static std::size_t CacheIndex(ProjectFileScanMode mode);
  static bool IsGitMetadataRelativePath(const std::filesystem::path& path);
  static bool IsHiddenRelativePath(const std::filesystem::path& path);
  static bool LessProjectPath(const ProjectFile& lhs, const std::filesystem::path& rhs);
  static bool LessProjectFile(const ProjectFile& lhs, const ProjectFile& rhs);
  static ProjectFile ToProjectFile(const platform::IndexFileEntry& entry);
  void EnsureFresh(ProjectFileScanMode mode) const;
  bool UpsertProjectFileLocked(const ProjectFile& file);
  bool RemoveProjectFileLocked(const std::filesystem::path& relative_path);
  void RebuildCacheLocked(ProjectFileScanMode mode, CacheBucket& cache) const;

  std::filesystem::path root_;
  mutable std::shared_mutex files_mutex_;
  std::vector<ProjectFile> files_;
  std::uint64_t version_ = 0;
  mutable CacheBucket exclude_hidden_cache_;
  mutable CacheBucket include_hidden_cache_;
};

}  // namespace microide::project
