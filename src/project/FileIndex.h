#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
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

// Immutable, shared list of relative paths produced by FileIndex.
using SharedPathList = std::shared_ptr<const std::vector<std::filesystem::path>>;

struct FilePathSnapshot {
  std::uint64_t version = 0;
  // Shared with the FileIndex cache: consumers can iterate without copying. The
  // pointer stays valid for as long as the consumer holds the snapshot, even if
  // the cache is rebuilt under them (a rebuild swaps in a new shared_ptr).
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
  // Pure full-tree scan + per-file stat, split out so a forced refresh can run it
  // on a background thread (it touches no FileIndex state — only its arguments):
  // the shell captures root/follow/excludes by value, scans off the UI thread, and
  // hands the sorted result to ReplaceScannedFiles() back on the main thread. This
  // keeps the whole-project `is_directory`/`file_size`/`last_write_time` sweep
  // (TD-2026-07-17-081/082) off the shell thread on manual refresh / exclude edits.
  // `out_status` (optional) reports why (if at all) the underlying tree walk
  // returned only a prefix, so a forced refresh can carry that status to
  // ReplaceScannedFiles() instead of silently dropping it (TD-2026-07-17-008/033).
  static std::vector<ProjectFile> ScanFiles(const std::filesystem::path& root,
                                            bool follow_out_of_root_symlinks,
                                            const std::vector<std::string>& exclude_globs,
                                            ProjectFileScanStatus* out_status = nullptr);
  // Commits a pre-scanned (already sorted) file list produced by ScanFiles(),
  // replacing the current contents. Runs on the owning (main) thread. `status` is
  // the completeness status reported by ScanFiles() for this same scan.
  void ReplaceScannedFiles(std::vector<ProjectFile> files, ProjectFileScanStatus status);
  // Mirrors the `project.follow_out_of_root_symlinks` user setting; consulted by
  // the full rescan in Refresh(). Default false keeps the out-of-root containment
  // guard active.
  void SetFollowOutOfRootSymlinks(bool follow) {
    follow_out_of_root_symlinks_.store(follow, std::memory_order_relaxed);
  }
  bool FollowOutOfRootSymlinks() const {
    return follow_out_of_root_symlinks_.load(std::memory_order_relaxed);
  }
  // User/project-configured ignore globs folded into the full-rescan (Refresh) via
  // CollectProjectFiles, alongside the built-in defaults. The background watcher
  // carries its own copy (FileIndexWatcher::SetExcludeGlobs); this covers the
  // manual ScanNow/Refresh path.
  void SetExcludeGlobs(std::vector<std::string> globs);
  // True when the last population of the index (the initial watcher batch or a
  // full ScanFiles/Refresh rescan) returned only a prefix of the tree: the index
  // therefore must not be presented as an authoritative complete file set
  // (TD-2026-07-17-008/033). `scan_status()` reports the specific cause(s).
  bool truncated() const;
  ProjectFileScanStatus scan_status() const;
  // Applies an index update batch. For the initial full-scan batch (the only
  // expensive case), `is_cancelled` is polled during the bulk rebuild; if it
  // returns true the rebuild aborts before committing, leaving the index
  // unchanged and returning false. This lets a teardown (StopFileIndexWatcher)
  // abandon an in-flight initial load instead of blocking on it.
  bool ApplyBatch(const platform::IndexUpdateBatch& batch,
                  const std::function<bool()>& is_cancelled = {});
  std::vector<ProjectFile> Snapshot() const;
  FileIndexSnapshot SnapshotWithVersion() const;
  FilePathSnapshot SnapshotPathsWithVersion(
      ProjectFileScanMode mode = ProjectFileScanMode::ExcludeHidden) const;
  std::vector<std::filesystem::path> SnapshotPaths(
      ProjectFileScanMode mode = ProjectFileScanMode::ExcludeHidden) const;
  std::uint64_t version() const;

  const std::filesystem::path& root() const { return root_; }

 private:
  struct CacheBucket {
    SharedPathList files;
    bool needs_refresh = true;
  };

  static std::size_t CacheIndex(ProjectFileScanMode mode);
  static bool IsGitMetadataRelativePath(const std::filesystem::path& path);
  static bool IsHiddenRelativePath(const std::filesystem::path& path);
  // True for an in-flight atomic-write staging temp (util::UniqueTemporaryPath).
  // Excluded from every snapshot in both scan modes: it exists only between a
  // save's write and its rename, so surfacing it in the finder or the search
  // candidate set only ever shows the user a file that is already gone.
  static bool IsTemporaryStagingRelativePath(const std::filesystem::path& path);
  static bool LessProjectPath(const ProjectFile& lhs, const std::filesystem::path& rhs);
  static bool LessProjectFile(const ProjectFile& lhs, const ProjectFile& rhs);
  static ProjectFile ToProjectFile(const platform::IndexFileEntry& entry);
  void EnsureFresh(ProjectFileScanMode mode) const;
  bool UpsertProjectFileLocked(const ProjectFile& file);
  bool RemoveProjectFileLocked(const std::filesystem::path& relative_path);
  // Remove the directory `relative_dir` and every indexed file beneath it. Used for
  // recursive deletions (a watched/nested directory removed or moved out).
  bool RemoveProjectSubtreeLocked(const std::filesystem::path& relative_dir);
  void RebuildCacheLocked(ProjectFileScanMode mode, CacheBucket& cache) const;

  std::filesystem::path root_;
  std::atomic<bool> follow_out_of_root_symlinks_{false};
  mutable std::shared_mutex files_mutex_;
  std::vector<std::string> exclude_globs_;   // guarded by files_mutex_
  ProjectFileScanStatus scan_status_;        // guarded by files_mutex_
  std::vector<ProjectFile> files_;
  std::uint64_t version_ = 0;
  mutable CacheBucket exclude_hidden_cache_;
  mutable CacheBucket include_hidden_cache_;
};

}  // namespace microide::project
