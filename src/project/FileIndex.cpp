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
  files_ = std::move(other.files_);
  exclude_hidden_cache_ = std::move(other.exclude_hidden_cache_);
  include_hidden_cache_ = std::move(other.include_hidden_cache_);
}

FileIndex& FileIndex::operator=(FileIndex&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  std::scoped_lock lock(files_mutex_, other.files_mutex_);
  root_ = std::move(other.root_);
  files_ = std::move(other.files_);
  exclude_hidden_cache_ = std::move(other.exclude_hidden_cache_);
  include_hidden_cache_ = std::move(other.include_hidden_cache_);
  return *this;
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

  root_ = absolute_root;
  {
    std::unique_lock lock(files_mutex_);
    files_.clear();
  }
  exclude_hidden_cache_ = {};
  include_hidden_cache_ = {};
  if (population_mode == RootPopulationMode::ScanNow) {
    Refresh();
  }
  return true;
}

void FileIndex::Reset() {
  root_.clear();
  {
    std::unique_lock lock(files_mutex_);
    files_.clear();
  }
  exclude_hidden_cache_ = {};
  include_hidden_cache_ = {};
}

void FileIndex::Refresh() {
  std::vector<ProjectFile> rebuilt;
  if (!root_.empty()) {
    const auto scanned = CollectProjectFiles(root_, ProjectFileScanMode::IncludeHidden);
    rebuilt.reserve(scanned.size());
    for (const auto& relative_path : scanned) {
      rebuilt.push_back(BuildProjectFile(root_, relative_path));
    }
  }

  {
    std::unique_lock lock(files_mutex_);
    files_ = std::move(rebuilt);
    std::sort(files_.begin(), files_.end(), LessProjectFile);
    exclude_hidden_cache_.needs_refresh = true;
    include_hidden_cache_.needs_refresh = true;
  }
  EnsureFresh(ProjectFileScanMode::ExcludeHidden);
}

bool FileIndex::ApplyBatch(const platform::IndexUpdateBatch& batch) {
  util::PerformanceTrace::Scope perf_scope("FileIndex::ApplyBatch");
  if (batch.is_initial) {
    util::PerformanceTrace::Scope initial_scope("FileIndex::ApplyBatch::InitialBulkLoad");
    std::vector<ProjectFile> rebuilt;
    rebuilt.reserve(batch.changes.size());
    for (const auto& change : batch.changes) {
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

    std::sort(rebuilt.begin(), rebuilt.end(), LessProjectFile);
    rebuilt.erase(std::unique(rebuilt.begin(), rebuilt.end(),
                              [](const ProjectFile& lhs, const ProjectFile& rhs) {
                                return lhs.relative_path == rhs.relative_path;
                              }),
                  rebuilt.end());

    std::unique_lock lock(files_mutex_);
    if (files_ == rebuilt) {
      return false;
    }
    files_ = std::move(rebuilt);
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
      changed = RemoveProjectFileLocked(relative_path) || changed;
      continue;
    }
    ProjectFile file = ToProjectFile(change.entry);
    file.relative_path = relative_path;
    changed = UpsertProjectFileLocked(file) || changed;
  }
  if (!changed) {
    return false;
  }
  exclude_hidden_cache_.needs_refresh = true;
  include_hidden_cache_.needs_refresh = true;
  return true;
}

std::vector<ProjectFile> FileIndex::Snapshot() const {
  std::shared_lock lock(files_mutex_);
  return files_;
}

const std::vector<std::filesystem::path>& FileIndex::files(ProjectFileScanMode mode) const {
  EnsureFresh(mode);
  return CacheIndex(mode) == 0 ? exclude_hidden_cache_.files : include_hidden_cache_.files;
}

std::size_t FileIndex::CacheIndex(ProjectFileScanMode mode) {
  return mode == ProjectFileScanMode::ExcludeHidden ? 0u : 1u;
}

bool FileIndex::IsHiddenRelativePath(const std::filesystem::path& path) {
  for (const auto& part : path) {
    const std::string name = part.string();
    if (!name.empty() && name[0] == '.') {
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

void FileIndex::RebuildCacheLocked(ProjectFileScanMode mode, CacheBucket& cache) const {
  util::PerformanceTrace::Scope perf_scope("FileIndex::RebuildCacheLocked");
  cache.files.clear();
  if (root_.empty()) {
    cache.needs_refresh = false;
    return;
  }

  cache.files.reserve(files_.size());
  for (const auto& file : files_) {
    if (mode == ProjectFileScanMode::ExcludeHidden && IsHiddenRelativePath(file.relative_path)) {
      continue;
    }
    cache.files.push_back(file.relative_path);
  }
  cache.needs_refresh = false;
}

}  // namespace microide::project
