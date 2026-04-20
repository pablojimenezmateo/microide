#include "project/FileIndex.h"

#include <system_error>

#include "project/ProjectFileScanner.h"
#include "util/StartupTrace.h"

namespace microide::project {

bool FileIndex::SetRoot(const std::filesystem::path& root) {
  util::StartupTrace::Scope trace_scope("FileIndex::SetRoot");
  std::error_code error;
  const auto absolute_root = std::filesystem::absolute(root, error);
  if (error || !std::filesystem::exists(absolute_root) ||
      !std::filesystem::is_directory(absolute_root)) {
    return false;
  }

  root_ = absolute_root;
  exclude_hidden_cache_ = {};
  include_hidden_cache_ = {};
  return true;
}

void FileIndex::Refresh() {
  exclude_hidden_cache_.needs_refresh = true;
  include_hidden_cache_.needs_refresh = true;
  EnsureFresh(ProjectFileScanMode::ExcludeHidden);
}

const std::vector<std::filesystem::path>& FileIndex::files(ProjectFileScanMode mode) const {
  EnsureFresh(mode);
  return CacheIndex(mode) == 0 ? exclude_hidden_cache_.files : include_hidden_cache_.files;
}

std::size_t FileIndex::CacheIndex(ProjectFileScanMode mode) {
  return mode == ProjectFileScanMode::ExcludeHidden ? 0u : 1u;
}

void FileIndex::EnsureFresh(ProjectFileScanMode mode) const {
  util::StartupTrace::Scope trace_scope("FileIndex::Refresh");
  CacheBucket& cache = CacheIndex(mode) == 0 ? exclude_hidden_cache_ : include_hidden_cache_;
  if (!cache.needs_refresh) {
    return;
  }

  cache.files.clear();
  if (root_.empty()) {
    cache.needs_refresh = false;
    return;
  }

  cache.files = CollectProjectFiles(root_, mode);
  cache.needs_refresh = false;
}

}  // namespace microide::project
