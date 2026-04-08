#include "project/FileIndex.h"

#include <algorithm>
#include <system_error>

#include "project/IgnoreMatcher.h"
#include "util/StartupTrace.h"

namespace microide::project {

namespace {

bool IsHiddenName(const std::filesystem::path& path) {
  const auto name = path.filename().string();
  return !name.empty() && name[0] == '.';
}

void CollectFiles(const std::filesystem::path& root,
                  const std::filesystem::path& directory,
                  const IgnoreMatcher& matcher,
                  std::vector<std::filesystem::path>& files) {
  std::error_code error;
  std::filesystem::directory_iterator iterator(
      directory, std::filesystem::directory_options::skip_permission_denied, error);
  std::filesystem::directory_iterator end;

  while (!error && iterator != end) {
    const auto path = iterator->path();
    const bool is_directory = iterator->is_directory();

    if (is_directory && path.filename() == ".git") {
      ++iterator;
      continue;
    }

    const auto relative = std::filesystem::relative(path, root, error);
    if (error || relative.empty()) {
      error.clear();
      ++iterator;
      continue;
    }

    if (IsHiddenName(path)) {
      ++iterator;
      continue;
    }

    if (is_directory) {
      IgnoreMatcher child_matcher = matcher;
      child_matcher.LoadIgnoreFile(path / ".gitignore");
      if (child_matcher.Ignored(relative, true)) {
        ++iterator;
        continue;
      }
      CollectFiles(root, path, child_matcher, files);
      ++iterator;
      continue;
    }

    if (matcher.Ignored(relative, false)) {
      ++iterator;
      continue;
    }

    if (iterator->is_regular_file()) {
      files.push_back(relative.lexically_normal());
    }
    ++iterator;
  }
}

}  // namespace

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

  IgnoreMatcher matcher;
  matcher.SetRoot(root_);
  CollectFiles(root_, root_, matcher, files_);

  std::sort(files_.begin(), files_.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.native() < rhs.native();
  });
  needs_refresh_ = false;
}

}  // namespace microide::project
