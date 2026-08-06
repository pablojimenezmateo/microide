#include "project/FileIndex.h"

#include "util/PathMatch.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <system_error>

#include "platform/HostPlatform.h"
#include "project/ProjectFileScanner.h"
#include "util/DurableFile.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StartupTrace.h"

namespace microide::project {

namespace {

// A batch-supplied "relative" path that is absolute or begins with ".." escapes
// the project root once resolved as `root / relative`. Native watchers, poll
// diffs, or hostile/test batches can present such paths; reject them so the index
// (and every file-finder/search/open that resolves through it) stays contained.
bool BatchPathEscapesRoot(const std::filesystem::path& relative_path) {
  if (relative_path.is_absolute()) {
    return true;
  }
  const auto it = relative_path.begin();
  return it != relative_path.end() && *it == std::filesystem::path("..");
}

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
  scan_status_ = other.scan_status_;
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
  scan_status_ = other.scan_status_;
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
  return scan_status_.incomplete();
}

ProjectFileScanStatus FileIndex::scan_status() const {
  std::shared_lock lock(files_mutex_);
  return scan_status_;
}

bool FileIndex::SetRoot(const std::filesystem::path& root,
                        RootPopulationMode population_mode) {
  util::StartupTrace::Scope trace_scope("FileIndex::SetRoot");
  std::error_code error;
  const auto absolute_root = std::filesystem::absolute(root, error);
  // Non-throwing probes: selecting a root that is inaccessible, a symlink cycle,
  // or on a flaky/unmounted volume must fail the open cleanly rather than throw
  // out of sidebar/index setup.
  if (error || !std::filesystem::exists(absolute_root, error) || error ||
      !std::filesystem::is_directory(absolute_root, error) || error) {
    return false;
  }

  {
    std::unique_lock lock(files_mutex_);
    root_ = absolute_root;
    files_.clear();
    // A previous root may have hit the entry budget; the new root has not been
    // scanned yet, so clear the stale truncation status (Refresh/ApplyBatch below
    // reassigns it from the real scan).
    scan_status_ = {};
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
  scan_status_ = {};
  ++version_;
  exclude_hidden_cache_ = {};
  include_hidden_cache_ = {};
}

std::vector<ProjectFile> FileIndex::ScanFiles(const std::filesystem::path& root,
                                              bool follow_out_of_root_symlinks,
                                              const std::vector<std::string>& exclude_globs,
                                              ProjectFileScanStatus* out_status) {
  if (out_status != nullptr) {
    *out_status = ProjectFileScanStatus{};
  }
  std::vector<ProjectFile> rebuilt;
  if (!root.empty()) {
    const auto scanned = CollectProjectFiles(root, ProjectFileScanMode::IncludeHidden,
                                             follow_out_of_root_symlinks, exclude_globs,
                                             out_status);
    rebuilt.reserve(scanned.size());
    for (const auto& relative_path : scanned) {
      rebuilt.push_back(BuildProjectFile(root, relative_path));
    }
  }
  std::sort(rebuilt.begin(), rebuilt.end(), LessProjectFile);
  return rebuilt;
}

void FileIndex::ReplaceScannedFiles(std::vector<ProjectFile> files, ProjectFileScanStatus status) {
  {
    std::unique_lock lock(files_mutex_);
    files_ = std::move(files);
    // A full rescan replaces the file list, so it also replaces the completeness
    // status: adopt this scan's outcome rather than leaving a prior root's
    // watcher-batch "truncated" state asserted over a freshly scanned (often
    // smaller) tree. `status` records why (if at all) the scan returned a prefix
    // (TD-2026-07-17-008/033).
    scan_status_ = status;
    ++version_;
    exclude_hidden_cache_.needs_refresh = true;
    include_hidden_cache_.needs_refresh = true;
  }
  EnsureFresh(ProjectFileScanMode::ExcludeHidden);
}

void FileIndex::Refresh() {
  std::filesystem::path root;
  std::vector<std::string> excludes;
  bool follow = false;
  {
    std::shared_lock lock(files_mutex_);
    root = root_;
    excludes = exclude_globs_;
    follow = follow_out_of_root_symlinks_.load(std::memory_order_relaxed);
  }
  ProjectFileScanStatus status;
  std::vector<ProjectFile> scanned = ScanFiles(root, follow, excludes, &status);
  ReplaceScannedFiles(std::move(scanned), status);
}

bool FileIndex::ApplyBatch(const platform::IndexUpdateBatch& batch,
                           const std::function<bool()>& is_cancelled) {
  util::PerformanceTrace::Scope perf_scope("FileIndex::ApplyBatch");
  util::AddPerformanceCounter(util::PerfCounterId::FileIndexApplyBatchCalls);
  util::AddPerformanceCounter(util::PerfCounterId::FileWatcherEventsCoalesced,
                              batch.changes.size());
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
      if (relative_path.empty() || IsGitMetadataRelativePath(relative_path) ||
          BatchPathEscapesRoot(relative_path)) {
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
    // The watcher's initial batch only truncates on the entry budget (BuildInitialBatch),
    // so map its bool onto the budget cause (TD-2026-07-17-008/033).
    scan_status_ = ProjectFileScanStatus{.truncated_by_budget = batch.truncated};
    exclude_hidden_cache_.needs_refresh = true;
    include_hidden_cache_.needs_refresh = true;
    return true;
  }

  std::unique_lock lock(files_mutex_);
  bool changed = false;
  for (const auto& change : batch.changes) {
    const std::filesystem::path relative_path = change.entry.relative_path.lexically_normal();
    if (relative_path.empty() || IsGitMetadataRelativePath(relative_path) ||
        BatchPathEscapesRoot(relative_path)) {
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

std::uint64_t FileIndex::VisitRelativePaths(
    ProjectFileScanMode mode,
    const std::function<void(const std::filesystem::path&)>& visit) const {
  util::PerformanceTrace::Scope perf_scope("FileIndex::VisitRelativePaths");
  std::shared_lock lock(files_mutex_);
  if (visit) {
    for (const ProjectFile& file : files_) {
      // The same two filters RebuildCacheLocked applies, and for the same
      // reasons: hidden paths are a mode question, and an in-flight atomic-write
      // staging temp exists only between a save's write and its rename, so
      // surfacing it only ever shows the user a file that is already gone.
      if (mode == ProjectFileScanMode::ExcludeHidden && IsHiddenRelativePath(file.relative_path)) {
        continue;
      }
      if (IsTemporaryStagingRelativePath(file.relative_path)) {
        continue;
      }
      visit(file.relative_path);
    }
  }
  return version_;
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
  if (it == path.end()) {
    return false;
  }
  const std::string first = it->string();
  if (first == ".git") {
    return true;
  }
  // On a case-insensitive host (Windows / default macOS), `.GIT`, `.Git`, or
  // watcher casing drift name the same metadata directory and must also be
  // excluded, or repository internals can leak into the finder / search index.
  if (platform::HostPathsAreCaseInsensitive() && first.size() == 4) {
    return (first[0] == '.') &&
           (first[1] == 'g' || first[1] == 'G') &&
           (first[2] == 'i' || first[2] == 'I') &&
           (first[3] == 't' || first[3] == 'T');
  }
  return false;
}

bool FileIndex::IsTemporaryStagingRelativePath(const std::filesystem::path& path) {
  return util::IsTemporaryStagingFilename(path.filename().string());
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
    // Both sides are already-normalized project-relative paths, so "at or below" is
    // a string-prefix question. The previous lexically_relative form answered the
    // same question but built a temporary path for EVERY indexed file on every
    // directory removal; the shared helper is allocation-free.
    return util::NormalizedPathEqualsOrWithin(file.relative_path, relative_dir);
  });
  return files_.size() != before;
}

void FileIndex::RebuildCacheLocked(ProjectFileScanMode mode, CacheBucket& cache) const {
  util::PerformanceTrace::Scope perf_scope("FileIndex::RebuildCacheLocked");
  util::AddPerformanceCounter(util::PerfCounterId::FileIndexRebuilds);
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
    // Every document save stages a temp beside its target inside the project tree
    // and renames it into place. A watcher batch that lands in that window would
    // otherwise leave a phantom entry in the file finder and in the project-search
    // candidate set until the next full rescan. Filtered here (rather than only at
    // the batch sites) so the full-scan path is covered too, and in both modes so
    // "include hidden files" does not expose them either.
    if (IsTemporaryStagingRelativePath(file.relative_path)) {
      continue;
    }
    rebuilt->push_back(file.relative_path);
  }
  cache.files = std::shared_ptr<const std::vector<std::filesystem::path>>(std::move(rebuilt));
  cache.needs_refresh = false;
}

}  // namespace microide::project
