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
  files_.clear();
  needs_refresh_ = true;
  return true;
}

void FileIndex::Refresh() {
  needs_refresh_ = true;
  EnsureFresh();
}

const std::vector<std::filesystem::path>& FileIndex::files() const {
  EnsureFresh();
  return files_;
}

void FileIndex::EnsureFresh() const {
  util::StartupTrace::Scope trace_scope("FileIndex::Refresh");
  if (!needs_refresh_) {
    return;
  }

  files_.clear();
  if (root_.empty()) {
    needs_refresh_ = false;
    return;
  }

  files_ = CollectProjectFiles(root_, ProjectFileScanMode::ExcludeHidden);
  needs_refresh_ = false;
}

}  // namespace microide::project
