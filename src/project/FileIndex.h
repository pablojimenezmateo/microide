#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "platform/FileIndexWatcher.h"
#include "project/PendingPaths.h"
#include "project/ProjectFileScanner.h"

namespace microide::project {

struct ProjectFile {
  std::filesystem::path relative_path;
  std::filesystem::file_time_type mtime{};
  std::uintmax_t size = 0;

  bool operator==(const ProjectFile&) const = default;
};

using SharedPathList = std::shared_ptr<const std::vector<std::string>>;

struct FilePathSnapshot {
  std::uint64_t version = 0;
  SharedPathList files;
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
  static std::vector<ProjectFile> ScanFiles(const std::filesystem::path& root,
                                            bool follow_out_of_root_symlinks,
                                            const std::vector<std::string>& exclude_globs,
                                            ProjectFileScanStatus* out_status = nullptr);
  void ReplaceScannedFiles(std::vector<ProjectFile> files, ProjectFileScanStatus status);
  bool ReplaceScannedFilesReportingChanges(
      std::vector<ProjectFile> files, ProjectFileScanStatus status,
      std::vector<platform::IndexUpdateBatch::Change>* out_changes);
  void SetFollowOutOfRootSymlinks(bool follow) {
    follow_out_of_root_symlinks_.store(follow, std::memory_order_relaxed);
  }
  bool FollowOutOfRootSymlinks() const {
    return follow_out_of_root_symlinks_.load(std::memory_order_relaxed);
  }
  void SetExcludeGlobs(std::vector<std::string> globs);
  bool truncated() const;
  ProjectFileScanStatus scan_status() const;
  bool ApplyBatch(const platform::IndexUpdateBatch& batch,
                  const std::function<bool()>& is_cancelled = {});
  std::vector<ProjectFile> Snapshot() const;
  FilePathSnapshot SnapshotPathsWithVersion(
      ProjectFileScanMode mode = ProjectFileScanMode::ExcludeHidden) const;
  std::vector<std::filesystem::path> SnapshotPaths(
      ProjectFileScanMode mode = ProjectFileScanMode::ExcludeHidden) const;
  std::uint64_t VisitRelativePaths(
      ProjectFileScanMode mode,
      const std::function<void(const std::filesystem::path&)>& visit) const;
  void SetPendingPaths(SharedPendingPaths pending);
  std::uint64_t version() const;

  const std::filesystem::path& root() const { return root_; }

 private:
  void TakeStateFrom(FileIndex& other);
  struct CacheBucket {
    SharedPathList files;
    bool needs_refresh = true;
  };

  static std::size_t CacheIndex(ProjectFileScanMode mode);
  static bool IsGitMetadataRelativePath(const std::filesystem::path& path);
  static bool IsHiddenRelativePath(const std::filesystem::path& path);
  static bool IsTemporaryStagingRelativePath(const std::filesystem::path& path);
  struct BatchOp {
    std::optional<ProjectFile> file;
    std::filesystem::path deleted_path;
    bool recursive = false;
    const std::filesystem::path& path() const {
      return file.has_value() ? file->relative_path : deleted_path;
    }
  };
  bool ApplySegmentLocked(std::vector<BatchOp*>& segment);
  static bool LessProjectPath(const ProjectFile& lhs, const std::filesystem::path& rhs);
  static bool LessProjectFile(const ProjectFile& lhs, const ProjectFile& rhs);
  static ProjectFile ToProjectFile(const platform::IndexFileEntry& entry,
                                   const std::filesystem::path& normalized_relative_path);
  void EnsureFresh(ProjectFileScanMode mode) const;
  bool RemoveProjectFileLocked(const std::filesystem::path& relative_path);
  bool RemoveProjectSubtreesLocked(const std::vector<std::filesystem::path>& relative_dirs);
  bool EraseIndexRangesLocked(std::vector<std::pair<std::size_t, std::size_t>>& ranges);
  void RebuildCacheLocked(ProjectFileScanMode mode, CacheBucket& cache) const;

  std::filesystem::path root_;
  std::atomic<bool> follow_out_of_root_symlinks_{false};
  mutable std::shared_mutex files_mutex_;
  std::vector<std::string> exclude_globs_;
  ProjectFileScanStatus scan_status_;
  std::vector<ProjectFile> files_;
  SharedPendingPaths pending_;
  std::uint64_t version_ = 0;
  mutable CacheBucket exclude_hidden_cache_;
  mutable CacheBucket include_hidden_cache_;
  std::mutex batch_scratch_mutex_;
  std::vector<BatchOp> batch_ops_;
  std::vector<BatchOp*> batch_segment_;
  std::vector<std::filesystem::path> batch_subtrees_;
};

}
