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
  // Everything cached per directory the walk enters. Both members answer a
  // question whose answer is the same for every entry in that directory, and both
  // used to be recomputed per ENTRY.
  struct DirectoryState {
    std::shared_ptr<const IgnoreMatcher> matcher;
    // Does any ancestor directory of an entry in this directory resolve to
    // ignored? `Includes` has to ask, because git does not let a file be
    // re-included out of an excluded directory — but the answer depends only on
    // the directory, and computing it per entry ran the whole rule set once per
    // path component of every file in the tree. Lazily filled on the first entry
    // seen here (kUnknown until then).
    signed char ancestors_ignored = kUnknown;
  };
  static constexpr signed char kUnknown = -1;

  std::shared_ptr<const IgnoreMatcher> MatcherForParentDirectory(
      const std::filesystem::path& directory);
  // Cache-hit fast path for the loop above: looks `directory_native` up in the
  // per-directory cache without building the `path` and key `std::string` the
  // general form needs, and falls back to it only on a miss (once per directory
  // per walk). Returns null for an entry that sits directly in the project root,
  // which has no ancestor chain to check and needs no cache entry; the caller uses
  // the root matcher then.
  DirectoryState* StateForParentDirectoryText(std::string_view directory_native,
                                              const std::filesystem::path& normalized_child);

  std::filesystem::path root_;
  // `root_.native()`, computed once. `Includes` compares a candidate's own text
  // against it per entry, and doing that through `path` would allocate. NATIVE
  // rather than generic because every text this class compares — the candidate,
  // the cache key, the parent view — is native, and mixing the two spellings
  // would make the prefix compare and the matcher cache silently miss on Windows.
  std::string root_native_;
#ifdef _WIN32
  // Separator-swapped relative text for IgnoreMatcher, which speaks generic. A
  // member so the buffer is reused across entries instead of reallocated per one;
  // POSIX needs no conversion at all and so carries no buffer.
  std::string generic_relative_scratch_;
#endif
  // Matchers are parent-linked (each holds only its own directory's rules and
  // shares the ancestor chain), so the cache stores shared pointers a descendant
  // can reference cheaply instead of a full copy of the inherited rule set per
  // directory (TD-2026-07-17A-055).
  std::shared_ptr<const IgnoreMatcher> root_matcher_;
  // Transparent so the per-entry lookup can key on a view into the candidate's own
  // text instead of materialising the directory's name once per filesystem entry.
  std::unordered_map<std::string, DirectoryState, util::TransparentStringHash,
                     std::equal_to<>>
      directory_states_;
};

}  // namespace microide::project
