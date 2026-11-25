#include "project/FileIndex.h"

#include "util/PathMatch.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <system_error>

#include "platform/Filesystem.h"
#include "project/ProjectFileScanner.h"
#include "util/DurableFile.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StartupTrace.h"

namespace microide::project {

namespace {

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
  const std::filesystem::path absolute_path = absolute_root / file.relative_path;
  if (const std::optional<platform::FileMetadata> metadata =
          platform::ReadFileMetadata(absolute_path);
      metadata && metadata->type == platform::PathType::RegularFile) {
    file.size = metadata->size;
    file.mtime = metadata->mtime;
  }
  return file;
}

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
  ++prefix_end.back();

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

}

FileIndex::FileIndex(FileIndex&& other) noexcept {
  std::unique_lock other_lock(other.files_mutex_);
  TakeStateFrom(other);
}

FileIndex& FileIndex::operator=(FileIndex&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(files_mutex_, other.files_mutex_);
  TakeStateFrom(other);
  return *this;
}

void FileIndex::TakeStateFrom(FileIndex& other) {
  root_ = std::move(other.root_);
  exclude_globs_ = std::move(other.exclude_globs_);
  scan_status_ = other.scan_status_;
  files_ = std::move(other.files_);
  pending_ = std::move(other.pending_);
  version_ = other.version_;
  follow_out_of_root_symlinks_.store(
      other.follow_out_of_root_symlinks_.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  exclude_hidden_cache_ = std::move(other.exclude_hidden_cache_);
  include_hidden_cache_ = std::move(other.include_hidden_cache_);
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
  if (error || !std::filesystem::exists(absolute_root, error) || error ||
      !std::filesystem::is_directory(absolute_root, error) || error) {
    return false;
  }

  {
    std::unique_lock lock(files_mutex_);
    root_ = absolute_root;
    files_.clear();
    pending_.reset();
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
  if (root_.empty() && files_.empty() && pending_ == nullptr) {
    return;
  }
  root_.clear();
  files_.clear();
  pending_.reset();
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
    util::StartupTrace::Scope startup_scope("FileIndex::InitialIndexReady");
    constexpr std::size_t kCancelCheckStride = 4096;
    std::size_t since_cancel_check = 0;
    std::vector<ProjectFile> rebuilt;
    rebuilt.reserve(batch.changes.size());
    std::filesystem::path normalize_scratch;
    for (const auto& change : batch.changes) {
      if (is_cancelled && ++since_cancel_check >= kCancelCheckStride) {
        since_cancel_check = 0;
        if (is_cancelled()) {
          return false;
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
      return false;
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
    scan_status_ = ProjectFileScanStatus{.truncated_by_budget = batch.truncated};
    exclude_hidden_cache_.needs_refresh = true;
    include_hidden_cache_.needs_refresh = true;
    return true;
  }

  const std::scoped_lock scratch_lock(batch_scratch_mutex_);
  std::vector<BatchOp>& ops = batch_ops_;
  ops.clear();
  ops.reserve(batch.changes.size());
  {
    std::filesystem::path normalize_scratch;
    for (const platform::IndexUpdateBatch::Change& change : batch.changes) {
      const std::filesystem::path& relative_path =
          util::NormalizedPathView(change.entry.relative_path, normalize_scratch);
      if (relative_path.empty() || IsGitMetadataRelativePath(relative_path) ||
          BatchPathEscapesRoot(relative_path)) {
        continue;
      }
      BatchOp& op = ops.emplace_back();
      if (change.kind == platform::IndexUpdateBatch::Kind::Deleted) {
        op.deleted_path = relative_path;
        op.recursive = change.recursive;
      } else {
        op.file = ToProjectFile(change.entry, relative_path);
      }
    }
  }

  std::unique_lock lock(files_mutex_);
  bool changed = false;
  std::vector<std::filesystem::path>& subtree_run = batch_subtrees_;
  std::vector<BatchOp*>& segment = batch_segment_;
  for (std::size_t i = 0; i < ops.size();) {
    if (ops[i].recursive) {
      subtree_run.clear();
      for (; i < ops.size() && ops[i].recursive; ++i) {
        subtree_run.push_back(std::move(ops[i].deleted_path));
      }
      changed = RemoveProjectSubtreesLocked(subtree_run) || changed;
      continue;
    }
    segment.clear();
    for (; i < ops.size() && !ops[i].recursive; ++i) {
      segment.push_back(&ops[i]);
    }
    changed = ApplySegmentLocked(segment) || changed;
  }
  ops.clear();
  segment.clear();
  subtree_run.clear();
  if (!changed) {
    return false;
  }
  ++version_;
  exclude_hidden_cache_.needs_refresh = true;
  include_hidden_cache_.needs_refresh = true;
  return true;
}

bool FileIndex::ApplySegmentLocked(std::vector<BatchOp*>& segment) {
  std::stable_sort(segment.begin(), segment.end(),
                   [](const BatchOp* lhs, const BatchOp* rhs) { return lhs->path().native() < rhs->path().native(); });
  std::size_t kept = 0;
  for (std::size_t j = 0; j < segment.size(); ++j) {
    if (j + 1 < segment.size() && segment[j + 1]->path().native() == segment[j]->path().native()) {
      continue;
    }
    segment[kept++] = segment[j];
  }
  segment.resize(kept);

  bool changed = false;
  std::size_t structural = 0;
  for (BatchOp*& op : segment) {
    const auto it = std::lower_bound(files_.begin(), files_.end(), op->path(), LessProjectPath);
    const bool present = it != files_.end() && it->relative_path == op->path();
    std::optional<ProjectFile>& file = op->file;
    if (file.has_value() && present) {
      if (it->mtime != file->mtime || it->size != file->size) {
        *it = std::move(*file);
        changed = true;
      }
      op = nullptr;
    } else if (!file.has_value() && !present) {
      op = nullptr;
    } else {
      ++structural;
    }
  }
  if (structural == 0) {
    return changed;
  }
  if (structural == 1) {
    for (BatchOp* op : segment) {
      if (op == nullptr) {
        continue;
      }
      if (op->file.has_value()) {
        files_.insert(std::lower_bound(files_.begin(), files_.end(), op->path(), LessProjectPath),
                      std::move(*op->file));
      } else {
        (void)RemoveProjectFileLocked(op->path());
      }
    }
    return true;
  }
  std::vector<ProjectFile> merged;
  merged.reserve(files_.size() + structural);
  std::size_t f = 0;
  for (BatchOp* op : segment) {
    if (op == nullptr) {
      continue;
    }
    while (f < files_.size() && files_[f].relative_path.native() < op->path().native()) {
      merged.push_back(std::move(files_[f++]));
    }
    if (op->file.has_value()) {
      merged.push_back(std::move(*op->file));
    } else {
      ++f;
    }
  }
  while (f < files_.size()) {
    merged.push_back(std::move(files_[f++]));
  }
  files_ = std::move(merged);
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
  if (!visit) {
    return version_;
  }
  const auto visit_file = [&](const std::filesystem::path& relative_path) {
    if (mode == ProjectFileScanMode::ExcludeHidden && IsHiddenRelativePath(relative_path)) {
      return;
    }
    if (IsTemporaryStagingRelativePath(relative_path)) {
      return;
    }
    visit(relative_path);
  };
  const std::vector<std::string> no_pending;
  const std::vector<std::string>& pending = pending_ != nullptr ? pending_->files() : no_pending;
  auto next_pending = pending.begin();
  for (const ProjectFile& file : files_) {
    for (; next_pending != pending.end() && *next_pending < file.relative_path.native(); ++next_pending) {
      visit_file(std::filesystem::path(*next_pending));
    }
    if (next_pending != pending.end() && *next_pending == file.relative_path.native()) {
      ++next_pending;
    }
    visit_file(file.relative_path);
  }
  for (; next_pending != pending.end(); ++next_pending) {
    visit_file(std::filesystem::path(*next_pending));
  }
  return version_;
}

void FileIndex::SetPendingPaths(SharedPendingPaths pending) {
  if (pending != nullptr && pending->empty()) {
    pending.reset();
  }
  std::unique_lock lock(files_mutex_);
  if (pending == pending_) {
    return;
  }
  pending_ = std::move(pending);
  ++version_;
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
  return it->native() == ".git";
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
  std::size_t write = ranges.front().first;
  std::size_t read = write;
  for (const std::pair<std::size_t, std::size_t>& range : ranges) {
    if (range.second <= read) {
      continue;
    }
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
  std::size_t out = 0;
  for (const auto& file : files_) {
    if (mode == ProjectFileScanMode::ExcludeHidden && IsHiddenRelativePath(file.relative_path)) {
      continue;
    }
    if (IsTemporaryStagingRelativePath(file.relative_path)) {
      continue;
    }
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

}
