#include "project/ProjectFileScanner.h"

#include <algorithm>
#include <system_error>

#include "project/IgnoreMatcher.h"

namespace microide::project {

namespace {

bool IsHiddenName(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  return !name.empty() && name[0] == '.';
}

void CollectFiles(const std::filesystem::path& root,
                  const std::filesystem::path& directory,
                  const IgnoreMatcher& matcher,
                  ProjectFileScanMode mode,
                  std::vector<std::filesystem::path>& files) {
  std::error_code error;
  std::filesystem::directory_iterator iterator(
      directory, std::filesystem::directory_options::skip_permission_denied, error);
  std::filesystem::directory_iterator end;

  while (!error && iterator != end) {
    const std::filesystem::path path = iterator->path();
    const bool is_directory = iterator->is_directory();

    if (is_directory && path.filename() == ".git") {
      ++iterator;
      continue;
    }

    const std::filesystem::path relative =
        path.lexically_normal().lexically_relative(root.lexically_normal());
    if (relative.empty()) {
      ++iterator;
      continue;
    }

    if (mode == ProjectFileScanMode::ExcludeHidden && IsHiddenName(path)) {
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
      CollectFiles(root, path, child_matcher, mode, files);
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

std::vector<std::filesystem::path> CollectProjectFiles(const std::filesystem::path& root,
                                                       ProjectFileScanMode mode) {
  std::error_code error;
  const std::filesystem::path absolute_root = std::filesystem::absolute(root, error);
  if (error || absolute_root.empty() || !std::filesystem::exists(absolute_root, error) || error ||
      !std::filesystem::is_directory(absolute_root, error)) {
    return {};
  }

  IgnoreMatcher matcher;
  matcher.SetRoot(absolute_root);

  std::vector<std::filesystem::path> files;
  CollectFiles(absolute_root, absolute_root, matcher, mode, files);
  std::sort(files.begin(), files.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.native() < rhs.native();
  });
  return files;
}

}  // namespace microide::project
