#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "platform/Filesystem.h"
#include "project/IgnoreMatcher.h"

namespace microide::project {

// Shared traversal-skip policy for every whole-tree walk in the app: the file
// index scan + watcher, the inotify registration, the sidebar directory tree,
// and the project file scanner. It seeds a root IgnoreMatcher with the built-in
// defaults (VCS metadata, dependency/cache trees, common build-output dirs) plus
// any user-configured exclude globs, then composes nested .gitignore files per
// directory (cached) so a decision at depth honors the full ancestor chain.
//
// Threading: the per-directory matcher cache is mutable and unsynchronized, so a
// single instance must be used by one walk at a time. Concurrent walks each get
// their own instance (callers already do this).
class ProjectTraversalFilter {
 public:
  explicit ProjectTraversalFilter(std::filesystem::path root,
                                   std::vector<std::string> extra_excludes = {});

  // Returns false when `path` (a directory or regular file at `type`) should be
  // pruned from indexing/watching. Directories that return false are also pruned
  // from recursion by the caller (disable_recursion_pending).
  bool Includes(const std::filesystem::path& path, platform::PathType type);

  // Convenience for the inotify registration path, which only ever asks about
  // directories.
  bool ShouldSkipDirectory(const std::filesystem::path& path) {
    return !Includes(path, platform::PathType::Directory);
  }

 private:
  std::filesystem::path RelativeToRoot(const std::filesystem::path& path) const;
  const IgnoreMatcher& MatcherForParentDirectory(const std::filesystem::path& directory);

  std::filesystem::path root_;
  IgnoreMatcher root_matcher_;
  std::unordered_map<std::string, IgnoreMatcher> directory_matchers_;
};

}  // namespace microide::project
