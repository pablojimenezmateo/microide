#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "platform/Filesystem.h"
#include "project/IgnoreMatcher.h"
#include "util/TransparentStringHash.h"

namespace microide::project {

class ProjectTraversalFilter {
 public:
  explicit ProjectTraversalFilter(std::filesystem::path root,
                                   std::vector<std::string> extra_excludes = {});

  bool Includes(const std::filesystem::path& path, platform::PathType type);
  bool Includes(std::string_view path_native, platform::PathType type);

  bool ShouldSkipDirectory(const std::filesystem::path& path) {
    return !Includes(path, platform::PathType::Directory);
  }

 private:
  bool IncludesNormalized(std::string_view normalized_text, platform::PathType type);

  struct DirectoryState {
    std::shared_ptr<const IgnoreMatcher> matcher;
    signed char ancestors_ignored = kUnknown;
  };
  static constexpr signed char kUnknown = -1;

  std::shared_ptr<const IgnoreMatcher> MatcherForParentDirectory(
      const std::filesystem::path& directory);
  DirectoryState* StateForParentDirectoryText(std::string_view directory_native);

  std::filesystem::path root_;
  std::string root_native_;
  std::shared_ptr<const IgnoreMatcher> root_matcher_;
  std::unordered_map<std::string, DirectoryState, util::TransparentStringHash,
                     std::equal_to<>>
      directory_states_;
};

}
