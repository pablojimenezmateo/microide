#include "project/FileIndex.h"

#include <algorithm>
#include <system_error>

#include "project/IgnoreMatcher.h"

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

    IgnoreMatcher child_matcher = matcher;
    if (is_directory) {
      child_matcher.LoadIgnoreFile(path / ".gitignore");
    }

    if (child_matcher.Ignored(relative, is_directory) || IsHiddenName(path)) {
      ++iterator;
      continue;
    }

    if (is_directory) {
      CollectFiles(root, path, child_matcher, files);
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
  std::error_code error;
  const auto absolute_root = std::filesystem::absolute(root, error);
  if (error || !std::filesystem::exists(absolute_root) ||
      !std::filesystem::is_directory(absolute_root)) {
    return false;
  }

  root_ = absolute_root;
  Refresh();
  return true;
}

void FileIndex::Refresh() {
  files_.clear();
  if (root_.empty()) {
    return;
  }

  IgnoreMatcher matcher;
  matcher.SetRoot(root_);
  CollectFiles(root_, root_, matcher, files_);

  std::sort(files_.begin(), files_.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.string() < rhs.string();
  });
}

}  // namespace microide::project
