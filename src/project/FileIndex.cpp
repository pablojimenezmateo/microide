#include "project/FileIndex.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <system_error>

#include "project/ProjectFileScanner.h"
#include "util/PerformanceTrace.h"
#include "util/StartupTrace.h"

namespace microide::project {

namespace {

ProjectFile BuildProjectFile(const std::filesystem::path& absolute_root,
                             const std::filesystem::path& relative_path) {
  ProjectFile file;
  file.relative_path = relative_path.lexically_normal();
  std::error_code error;
  const std::filesystem::path absolute_path = absolute_root / file.relative_path;
  const auto status = std::filesystem::status(absolute_path, error);
  if (!error && std::filesystem::is_regular_file(status)) {
    file.size = std::filesystem::file_size(absolute_path, error);
    if (error) {
      file.size = 0;
      error.clear();
    }
    file.mtime = std::filesystem::last_write_time(absolute_path, error);
    if (error) {
      file.mtime = {};
    }
  }
  return file;
}

}  // namespace

FileIndex::FileIndex(FileIndex&& other) noexcept {
  std::unique_lock other_lock(other.files_mutex_);
  root_ = std::move(other.root_);
  exclude_globs_ = std::move(other.exclude_globs_);
  truncated_ = other.truncated_;
  files_ = std::move(other.files_);
  version_ = other.version_;
  // The symlink-containment flag is per-project state applied at root config time,
  // not re-applied on activation — so it MUST survive the move (a project switch
  // move-assigns FileIndex). Dropping it silently reverts to containment-enforced,
  // hiding out-of-root-symlinked files for users who enabled following them.
  follow_out_of_root_symlinks_.store(
      other.follow_out_of_root_symlinks_.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  exclude_hidden_cache_ = std::move(other.exclude_hidden_cache_);
  include_hidden_cache_ = std::move(other.include_hidden_cache_);
}

FileIndex& FileIndex::operator=(FileIndex&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  std::scoped_lock lock(files_mutex_, other.files_mutex_);
  root_ = std::move(other.root_);
  exclude_globs_ = std::move(other.exclude_globs_);
  truncated_ = other.truncated_;
  files_ = std::move(other.files_);
  version_ = other.version_;
  // See the move constructor: this per-project flag must survive the move.
  follow_out_of_root_symlinks_.store(
      other.follow_out_of_root_symlinks_.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  exclude_hidden_cache_ = std::move(other.exclude_hidden_cache_);
  include_hidden_cache_ = std::move(other.include_hidden_cache_);
  return *this;
}

void FileIndex::SetExcludeGlobs(std::vector<std::string> globs) {
  std::unique_lock lock(files_mutex_);
  exclude_globs_ = std::move(globs);
}

bool FileIndex::truncated() const {
  std::shared_lock lock(files_mutex_);
  return truncated_;
}

bool FileIndex::SetRoot(const std::filesystem::path& root,
                        RootPopulationMode population_mode) {
  util::StartupTrace::Scope trace_scope("FileIndex::SetRoot");
  std::error_code error;
  const auto absolute_root = std::filesystem::absolute(root, error);
  if (error || !std::filesystem::exists(absolute_root) ||
      !std::filesystem::is_directory(absolute_root)) {
    return false;
  }

  {
    std::unique_lock lock(files_mutex_);
    root_ = absolute_root;
    files_.clear();
    ++version_;
    exclude_hidden_cache_ = {};
    include_hidden_cache_ = {};
  }
  if (population_mode == RootPopulationMode::ScanNow) {
    Refresh();
  }
  return true;
}

void FileIndex::Reset() {
  std::unique_lock lock(files_mutex_);
  if (root_.empty() && files_.empty()) {
    return;
  }
  root_.clear();
  files_.clear();
  ++version_;
  exclude_hidden_cache_ = {};
  include_hidden_cache_ = {};
}

void FileIndex::Refresh() {
  std::filesystem::path root;
  {
    std::shared_lock lock(files_mutex_);
    root = root_;
  }

  std::vector<std::string> excludes;
  {
    std::shared_lock lock(files_mutex_);
    excludes = exclude_globs_;
  }

  std::vector<ProjectFile> rebuilt;
  if (!root.empty()) {
    const auto scanned = CollectProjectFiles(
        root, ProjectFileScanMode::IncludeHidden,
        follow_out_of_root_symlinks_.load(std::memory_order_relaxed), excludes);
    rebuilt.reserve(scanned.size());
    for (const auto& relative_path : scanned) {
      rebuilt.push_back(BuildProjectFile(root, relative_path));
    }
  }
  std::sort(rebuilt.begin(), rebuilt.end(), LessProjectFile);

  {
    std::unique_lock lock(files_mutex_);
    files_ = std::move(rebuilt);
    ++version_;
    exclude_hidden_cache_.needs_refresh = true;
    include_hidden_cache_.needs_refresh = true;
  }
  EnsureFresh(ProjectFileScanMode::ExcludeHidden);
}

bool FileIndex::ApplyBatch(const platform::IndexUpdateBatch& batch,
                           const std::function<bool()>& is_cancelled) {
  util::PerformanceTrace::Scope perf_scope("FileIndex::ApplyBatch");
  if (batch.is_initial) {
    util::PerformanceTrace::Scope initial_scope("FileIndex::ApplyBatch::InitialBulkLoad");
    // Poll the cancellation predicate periodically (not every iteration) so an
    // abandoned load bails promptly without per-entry overhead.
    constexpr std::size_t kCancelCheckStride = 4096;
    std::size_t since_cancel_check = 0;
    std::vector<ProjectFile> rebuilt;
    rebuilt.reserve(batch.changes.size());
    for (const auto& change : batch.changes) {
      if (is_cancelled && ++since_cancel_check >= kCancelCheckStride) {
        since_cancel_check = 0;
        if (is_cancelled()) {
          return false;  // abandon before committing; index left unchanged
        }
      }
      if (change.kind != platform::IndexUpdateBatch::Kind::CreatedOrModified) {
        continue;
      }
      const std::filesystem::path relative_path = change.entry.relative_path.lexically_normal();
      if (relative_path.empty() || IsGitMetadataRelativePath(relative_path)) {
        continue;
      }
      ProjectFile file = ToProjectFile(change.entry);
      file.relative_path = relative_path;
      rebuilt.push_back(std::move(file));
    }

    if (is_cancelled && is_cancelled()) {
      return false;  // abandon before the (also non-trivial) sort/commit
    }
    std::sort(rebuilt.begin(), rebuilt.end(), LessProjectFile);
    rebuilt.erase(std::unique(rebuilt.begin(), rebuilt.end(),
                              [](const ProjectFile& lhs, const ProjectFile& rhs) {
                                return lhs.relative_path == rhs.relative_path;
                              }),
                  rebuilt.end());

    std::unique_lock lock(files_mutex_);
    files_ = std::move(rebuilt);
    ++version_;
    truncated_ = batch.truncated;
    exclude_hidden_cache_.needs_refresh = true;
    include_hidden_cache_.needs_refresh = true;
    return true;
  }

  std::unique_lock lock(files_mutex_);
  bool changed = false;
  for (const auto& change : batch.changes) {
    const std::filesystem::path relative_path = change.entry.relative_path.lexically_normal();
    if (relative_path.empty() || IsGitMetadataRelativePath(relative_path)) {
      continue;
    }
    if (change.kind == platform::IndexUpdateBatch::Kind::Deleted) {
      changed = (change.recursive ? RemoveProjectSubtreeLocked(relative_path)
                                  : RemoveProjectFileLocked(relative_path)) ||
                changed;
      continue;
    }
    ProjectFile file = ToProjectFile(change.entry);
    file.relative_path = relative_path;
    changed = UpsertProjectFileLocked(file) || changed;
  }
  if (!changed) {
    return false;
  }
  ++version_;
  exclude_hidden_cache_.needs_refresh = true;
  include_hidden_cache_.needs_refresh = true;
  return true;
}

std::vector<ProjectFile> FileIndex::Snapshot() const {
  std::shared_lock lock(files_mutex_);
  return files_;
}

FileIndexSnapshot FileIndex::SnapshotWithVersion() const {
  std::shared_lock lock(files_mutex_);
  return FileIndexSnapshot{
      .version = version_,
      .files = files_,
  };
}

FilePathSnapshot FileIndex::SnapshotPathsWithVersion(ProjectFileScanMode mode) const {
  std::unique_lock lock(files_mutex_);
  CacheBucket& cache = CacheIndex(mode) == 0 ? exclude_hidden_cache_ : include_hidden_cache_;
  if (cache.needs_refresh) {
    RebuildCacheLocked(mode, cache);
  }
  return FilePathSnapshot{
      .version = version_,
      .files = cache.files,
  };
}

std::vector<std::filesystem::path> FileIndex::SnapshotPaths(ProjectFileScanMode mode) const {
  const FilePathSnapshot snapshot = SnapshotPathsWithVersion(mode);
  return snapshot.files ? *snapshot.files : std::vector<std::filesystem::path>{};
}

const std::vector<std::filesystem::path>& FileIndex::files(ProjectFileScanMode mode) const {
  static const std::vector<std::filesystem::path> kEmpty;
  EnsureFresh(mode);
  const auto& bucket = CacheIndex(mode) == 0 ? exclude_hidden_cache_ : include_hidden_cache_;
  return bucket.files ? *bucket.files : kEmpty;
}

std::uint64_t FileIndex::version() const {
  std::shared_lock lock(files_mutex_);
  return version_;
}

std::size_t FileIndex::CacheIndex(ProjectFileScanMode mode) {
  return mode == ProjectFileScanMode::ExcludeHidden ? 0u : 1u;
}

bool FileIndex::IsHiddenRelativePath(const std::filesystem::path& path) {
  for (const auto& part : path) {
    // c_str()[0] reads the first character without allocating a std::string per
    // component; an empty component yields '\0', which is correctly not hidden.
    if (part.c_str()[0] == '.') {
      return true;
    }
  }
  return false;
}

bool FileIndex::IsGitMetadataRelativePath(const std::filesystem::path& path) {
  const auto it = path.begin();
  return it != path.end() && *it == std::filesystem::path(".git");
}

bool FileIndex::LessProjectPath(const ProjectFile& lhs, const std::filesystem::path& rhs) {
  return lhs.relative_path.native() < rhs.native();
}

bool FileIndex::LessProjectFile(const ProjectFile& lhs, const ProjectFile& rhs) {
  return lhs.relative_path.native() < rhs.relative_path.native();
}

ProjectFile FileIndex::ToProjectFile(const platform::IndexFileEntry& entry) {
  return ProjectFile{
      .relative_path = entry.relative_path.lexically_normal(),
      .mtime = entry.mtime,
      .size = entry.size,
  };
}

void FileIndex::EnsureFresh(ProjectFileScanMode mode) const {
  util::StartupTrace::Scope trace_scope("FileIndex::SnapshotCache");
  util::PerformanceTrace::Scope perf_scope("FileIndex::EnsureFresh");
  std::unique_lock lock(files_mutex_);
  CacheBucket& cache = CacheIndex(mode) == 0 ? exclude_hidden_cache_ : include_hidden_cache_;
  if (!cache.needs_refresh) {
    return;
  }
  RebuildCacheLocked(mode, cache);
}

bool FileIndex::UpsertProjectFileLocked(const ProjectFile& file) {
  auto it = std::lower_bound(files_.begin(), files_.end(), file.relative_path, LessProjectPath);
  if (it != files_.end() && it->relative_path == file.relative_path) {
    if (it->mtime == file.mtime && it->size == file.size) {
      return false;
    }
    *it = file;
    return true;
  }
  files_.insert(it, file);
  return true;
}

bool FileIndex::RemoveProjectFileLocked(const std::filesystem::path& relative_path) {
  auto it = std::lower_bound(files_.begin(), files_.end(), relative_path, LessProjectPath);
  if (it != files_.end() && it->relative_path == relative_path) {
    files_.erase(it);
    return true;
  }
  return false;
}

bool FileIndex::RemoveProjectSubtreeLocked(const std::filesystem::path& relative_dir) {
  const std::size_t before = files_.size();
  std::erase_if(files_, [&relative_dir](const ProjectFile& file) {
    // lexically_relative returns a path with no leading ".." exactly when file is at
    // or below relative_dir (the directory's own path yields ".", still under it).
    const std::filesystem::path rel = file.relative_path.lexically_relative(relative_dir);
    return !rel.empty() && *rel.begin() != "..";
  });
  return files_.size() != before;
}

void FileIndex::RebuildCacheLocked(ProjectFileScanMode mode, CacheBucket& cache) const {
  util::PerformanceTrace::Scope perf_scope("FileIndex::RebuildCacheLocked");
  auto rebuilt = std::make_shared<std::vector<std::filesystem::path>>();
  if (root_.empty()) {
    cache.files = std::shared_ptr<const std::vector<std::filesystem::path>>(std::move(rebuilt));
    cache.needs_refresh = false;
    return;
  }

  rebuilt->reserve(files_.size());
  for (const auto& file : files_) {
    if (mode == ProjectFileScanMode::ExcludeHidden && IsHiddenRelativePath(file.relative_path)) {
      continue;
    }
    rebuilt->push_back(file.relative_path);
  }
  cache.files = std::shared_ptr<const std::vector<std::filesystem::path>>(std::move(rebuilt));
  cache.needs_refresh = false;
}

}  // namespace microide::project
