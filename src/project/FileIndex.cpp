#include "project/FileIndex.h"

#include "util/PathMatch.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <system_error>

#include "platform/Filesystem.h"
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
  file.relative_path = util::NormalizedPath(relative_path);
  // One stat for the whole triple. This was `status()` to classify, then
  // `file_size()`, then `last_write_time()` -- three syscalls per file of a full
  // rescan, each re-resolving the same path.
  const std::filesystem::path absolute_path = absolute_root / file.relative_path;
  if (const std::optional<platform::FileMetadata> metadata =
          platform::ReadFileMetadata(absolute_path);
      metadata && metadata->type == platform::PathType::RegularFile) {
    file.size = metadata->size;
    file.mtime = metadata->mtime;
  }
  return file;
}

// `files` is sorted by `relative_path.native()`, so a directory's subtree is a
// CONTIGUOUS run: the entries beneath `dir` are exactly those with the byte prefix
// "dir/", and every string carrying that prefix sorts into [dir/, dir<sep+1>).
// Two binary searches bound it instead of a scan of the whole index.
//
// A directory covers at most TWO runs, not one: its own path (should a batch ever
// have indexed the directory itself as a file) sorts before the prefix run but is
// not adjacent to it -- "sub.txt" sorts between "sub" and "sub/a.cpp", because '.'
// (0x2E) precedes '/' (0x2F). Both are appended.
void AppendSubtreeRanges(const std::vector<ProjectFile>& files,
                         const std::filesystem::path& relative_dir,
                         std::vector<std::pair<std::size_t, std::size_t>>& out) {
  using Text = std::filesystem::path::string_type;
  const Text& dir_text = relative_dir.native();
  if (dir_text.empty()) {
    return;
  }
  const auto less_text = [](const ProjectFile& lhs, const Text& rhs) {
    return lhs.relative_path.native() < rhs;
  };
  const auto index_of = [&files](std::vector<ProjectFile>::const_iterator it) {
    return static_cast<std::size_t>(it - files.begin());
  };

  Text prefix;
  prefix.reserve(dir_text.size() + 1);
  prefix.assign(dir_text);
  prefix.push_back(static_cast<Text::value_type>(std::filesystem::path::preferred_separator));
  Text prefix_end = prefix;
  ++prefix_end.back();  // '/' -> '0': the first string past every "dir/" prefix.

  const auto subtree_first = std::lower_bound(files.begin(), files.end(), prefix, less_text);
  const auto subtree_last = std::lower_bound(subtree_first, files.end(), prefix_end, less_text);
  if (subtree_first != subtree_last) {
    out.emplace_back(index_of(subtree_first), index_of(subtree_last));
  }

  const auto exact = std::lower_bound(files.begin(), subtree_first, dir_text, less_text);
  if (exact != subtree_first && exact->relative_path.native() == dir_text) {
    out.emplace_back(index_of(exact), index_of(exact) + 1);
  }
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

bool FileIndex::ReplaceScannedFilesReportingChanges(
    std::vector<ProjectFile> files, ProjectFileScanStatus status,
    std::vector<platform::IndexUpdateBatch::Change>* out_changes) {
  {
    std::unique_lock lock(files_mutex_);
    // Both sides are sorted by LessProjectFile (ScanFiles sorts its result;
    // ApplyBatch upserts in order), so the difference is a single merge walk with
    // no allocation for the entries that are unchanged -- which, on the forced
    // path this serves, is essentially all of them.
    bool differs = false;
    const auto append_upsert = [&](const ProjectFile& file) {
      differs = true;
      if (out_changes == nullptr) {
        return;
      }
      platform::IndexUpdateBatch::Change change;
      change.kind = platform::IndexUpdateBatch::Kind::CreatedOrModified;
      change.entry.relative_path = file.relative_path;
      change.entry.mtime = file.mtime;
      change.entry.size = file.size;
      out_changes->push_back(std::move(change));
    };
    std::size_t previous_index = 0;
    std::size_t scanned_index = 0;
    while (previous_index < files_.size() || scanned_index < files.size()) {
      if (scanned_index == files.size() ||
          (previous_index < files_.size() &&
           LessProjectFile(files_[previous_index], files[scanned_index]))) {
        differs = true;
        if (out_changes != nullptr) {
          platform::IndexUpdateBatch::Change change;
          change.kind = platform::IndexUpdateBatch::Kind::Deleted;
          change.entry.relative_path = files_[previous_index].relative_path;
          out_changes->push_back(std::move(change));
        }
        ++previous_index;
        continue;
      }
      if (previous_index == files_.size() ||
          LessProjectFile(files[scanned_index], files_[previous_index])) {
        append_upsert(files[scanned_index]);
        ++scanned_index;
        continue;
      }
      if (files[scanned_index] != files_[previous_index]) {
        append_upsert(files[scanned_index]);
      }
      ++previous_index;
      ++scanned_index;
    }
    if (!differs && status == scan_status_) {
      // Byte-identical to what the index already holds: leave it — and its version
      // — alone, so a forced check on a quiet tree invalidates no derived cache.
      return false;
    }
    files_ = std::move(files);
    scan_status_ = status;
    ++version_;
    exclude_hidden_cache_.needs_refresh = true;
    include_hidden_cache_.needs_refresh = true;
  }
  EnsureFresh(ProjectFileScanMode::ExcludeHidden);
  return true;
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
    // Also a STARTUP scope, and the reason is its `ms total` column rather than
    // its duration: this is the moment the project index exists, so the file
    // finder, project search and every index-backed lookup start answering. The
    // startup channel measures from the top of Application::Initialize, so the
    // line reads as "the project was ready at T ms" — the launch number nothing
    // else reported. The perf channel above measures the same region but has no
    // origin, so it can only say how long the load took, not when it landed.
    util::StartupTrace::Scope startup_scope("FileIndex::InitialIndexReady");
    // Poll the cancellation predicate periodically (not every iteration) so an
    // abandoned load bails promptly without per-entry overhead.
    constexpr std::size_t kCancelCheckStride = 4096;
    std::size_t since_cancel_check = 0;
    std::vector<ProjectFile> rebuilt;
    rebuilt.reserve(batch.changes.size());
    // The batch's paths come out of the watcher's walk already normal, and
    // `lexically_normal()` is ~12 allocations whether or not it changes anything
    // (TD-2026-08-10-174). At 46,805 files that no-op ran twice per entry -- once
    // here and once more inside ToProjectFile, whose result the caller then threw
    // away. The scratch is written only for an oddly-spelled input.
    std::filesystem::path normalize_scratch;
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
      const std::filesystem::path& relative_path =
          util::NormalizedPathView(change.entry.relative_path, normalize_scratch);
      if (relative_path.empty() || IsGitMetadataRelativePath(relative_path) ||
          BatchPathEscapesRoot(relative_path)) {
        continue;
      }
      rebuilt.push_back(ToProjectFile(change.entry, relative_path));
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
  std::filesystem::path normalize_scratch;
  std::vector<std::filesystem::path> subtree_run;
  const auto accepted_path = [](const std::filesystem::path& relative_path) {
    return !relative_path.empty() && !IsGitMetadataRelativePath(relative_path) &&
           !BatchPathEscapesRoot(relative_path);
  };
  for (std::size_t i = 0; i < batch.changes.size(); ++i) {
    const platform::IndexUpdateBatch::Change& change = batch.changes[i];
    const std::filesystem::path& relative_path =
        util::NormalizedPathView(change.entry.relative_path, normalize_scratch);
    if (!accepted_path(relative_path)) {
      continue;
    }
    if (change.kind == platform::IndexUpdateBatch::Kind::Deleted) {
      if (!change.recursive) {
        changed = RemoveProjectFileLocked(relative_path) || changed;
        continue;
      }
      // Consume the whole consecutive run of recursive deletions and drop it in
      // one compaction. A `rm -rf`, a branch switch, or a build cleaning its
      // output arrives as exactly such a run -- one change per removed directory,
      // doubled because the parent watch and the directory's own watch each
      // report it -- and applying them one at a time shifts the tail of the index
      // vector once per change. They are idempotent and commute, so the run is
      // just the union of their ranges.
      subtree_run.clear();
      subtree_run.push_back(relative_path);
      while (i + 1 < batch.changes.size()) {
        const platform::IndexUpdateBatch::Change& next = batch.changes[i + 1];
        if (next.kind != platform::IndexUpdateBatch::Kind::Deleted || !next.recursive) {
          break;
        }
        ++i;
        // The scratch may be rewritten here, which is why the run holds copies.
        const std::filesystem::path& next_path =
            util::NormalizedPathView(next.entry.relative_path, normalize_scratch);
        if (accepted_path(next_path)) {
          subtree_run.push_back(next_path);
        }
      }
      changed = RemoveProjectSubtreesLocked(subtree_run) || changed;
      continue;
    }
    changed = UpsertProjectFileLocked(ToProjectFile(change.entry, relative_path)) || changed;
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
  std::vector<std::filesystem::path> paths;
  if (!snapshot.files) {
    return paths;
  }
  paths.reserve(snapshot.files->size());
  for (const std::string& text : *snapshot.files) {
    paths.emplace_back(text);
  }
  return paths;
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

ProjectFile FileIndex::ToProjectFile(const platform::IndexFileEntry& entry,
                                     const std::filesystem::path& normalized_relative_path) {
  return ProjectFile{
      .relative_path = normalized_relative_path,
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

bool FileIndex::RemoveProjectSubtreesLocked(
    const std::vector<std::filesystem::path>& relative_dirs) {
  if (relative_dirs.empty()) {
    return false;
  }
  // "." denotes the project root spelled as a relative path: every entry is within
  // it, and no byte prefix expresses that (util::NormalizedPathEqualsOrWithin
  // special-cases it for the same reason). One such entry subsumes the whole run.
  for (const std::filesystem::path& relative_dir : relative_dirs) {
    const std::filesystem::path::string_type& text = relative_dir.native();
    if (text.size() == 1 && text[0] == '.') {
      const std::size_t removed = files_.size();
      files_.clear();
      util::AddPerformanceCounter(util::PerfCounterId::FileIndexSubtreeRemovals,
                                  static_cast<std::uint64_t>(relative_dirs.size()));
      util::AddPerformanceCounter(util::PerfCounterId::FileIndexSubtreeEntriesRemoved,
                                  static_cast<std::uint64_t>(removed));
      return removed != 0;
    }
  }

  // Every range is computed against the UNMODIFIED vector, so they are all valid
  // together; the single compaction below applies their union.
  std::vector<std::pair<std::size_t, std::size_t>> ranges;
  ranges.reserve(relative_dirs.size() * 2);
  for (const std::filesystem::path& relative_dir : relative_dirs) {
    AppendSubtreeRanges(files_, relative_dir, ranges);
  }
  const std::size_t before = files_.size();
  const bool changed = EraseIndexRangesLocked(ranges);
  util::AddPerformanceCounter(util::PerfCounterId::FileIndexSubtreeRemovals,
                              static_cast<std::uint64_t>(relative_dirs.size()));
  util::AddPerformanceCounter(util::PerfCounterId::FileIndexSubtreeEntriesRemoved,
                              static_cast<std::uint64_t>(before - files_.size()));
  return changed;
}

bool FileIndex::EraseIndexRangesLocked(std::vector<std::pair<std::size_t, std::size_t>>& ranges) {
  if (ranges.empty()) {
    return false;
  }
  std::sort(ranges.begin(), ranges.end());
  // One left-to-right sweep: move each surviving block down over the gap the
  // ranges before it opened, then truncate. Erasing the ranges one at a time
  // would shift the vector's tail once per range instead of once in total.
  std::size_t write = ranges.front().first;
  std::size_t read = write;
  for (const std::pair<std::size_t, std::size_t>& range : ranges) {
    // A range that ends at or before the cursor is already erased -- ranges are
    // sorted, so this is an enclosed one. Skipping it is load-bearing, not a
    // shortcut: assigning `read` from it would rewind the cursor into ground the
    // enclosing range covered, and the tail copy would resurrect those entries.
    if (range.second <= read) {
      continue;
    }
    // No clamp needed on the low end: `range.first` may sit before the cursor when
    // two ranges overlap, and this loop is already bounded below by `read`.
    for (std::size_t i = read; i < range.first; ++i) {
      files_[write++] = std::move(files_[i]);
    }
    read = range.second;
  }
  for (std::size_t i = read; i < files_.size(); ++i) {
    files_[write++] = std::move(files_[i]);
  }
  if (write == files_.size()) {
    return false;
  }
  files_.resize(write);
  return true;
}

void FileIndex::RebuildCacheLocked(ProjectFileScanMode mode, CacheBucket& cache) const {
  util::PerformanceTrace::Scope perf_scope("FileIndex::RebuildCacheLocked");
  util::AddPerformanceCounter(util::PerfCounterId::FileIndexRebuilds);
  // Recycle the previous list when nothing else is holding it. A one-file change
  // invalidates the whole cache, and rebuilding into the old vector lets each
  // `std::string` assignment reuse the capacity already sized for that entry — so
  // the common case (one file changed, ten thousand entries identical) allocates
  // nothing at all instead of once per file. A consumer still holding the previous
  // snapshot (an in-flight search) keeps it: use_count says so, and we build fresh.
  // `use_count() == 1` is sound here in both directions. Every other owner is a
  // `FilePathSnapshot` handed out by SnapshotPathsWithVersion, which takes this
  // same exclusive lock, so no new owner can appear while we hold it — and an
  // owner going away concurrently only drops the count, which at worst makes us
  // read 2 and build fresh. The unsafe direction (reading 1 while a live consumer
  // still holds it) cannot happen, so const_pointer_cast has no observer to
  // surprise.
  std::shared_ptr<std::vector<std::string>> rebuilt;
  if (cache.files != nullptr && cache.files.use_count() == 1) {
    rebuilt = std::const_pointer_cast<std::vector<std::string>>(cache.files);
    cache.files.reset();
  } else {
    rebuilt = std::make_shared<std::vector<std::string>>();
  }
  if (root_.empty()) {
    rebuilt->clear();
    cache.files = std::shared_ptr<const std::vector<std::string>>(std::move(rebuilt));
    cache.needs_refresh = false;
    return;
  }

  rebuilt->reserve(files_.size());
  // Overwrite in place rather than clear-and-push: `clear()` on a vector of
  // strings runs every destructor, which hands back exactly the buffers the next
  // entry is about to ask for. Assigning into a live string reuses its capacity,
  // so an unchanged entry costs a memcpy and no allocation. The tail is trimmed
  // once at the end.
  std::size_t out = 0;
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
    // `native()`, not `generic_string()`: this is the POSIX host (see AGENTS.md
    // § non-Linux backends), where the native separator IS '/', so the two are
    // the same bytes — and native() returns a reference while generic_string()
    // returns a fresh std::string, which is the allocation this loop exists to
    // avoid. The paths were normalized on ingress by MakeGitRepositoryPathIdentity
    // / NormalizedPathView, so no separator folding is owed here either.
    if (out < rebuilt->size()) {
      (*rebuilt)[out].assign(file.relative_path.native());
    } else {
      rebuilt->emplace_back(file.relative_path.native());
    }
    ++out;
  }
  rebuilt->resize(out);
  cache.files = std::shared_ptr<const std::vector<std::string>>(std::move(rebuilt));
  cache.needs_refresh = false;
}

}  // namespace microide::project
