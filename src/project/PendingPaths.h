#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "util/TransparentStringHash.h"

namespace microide::project {

// Files a project's file set lists that its tree on disk does not hold yet: a
// remote mirror's paths its first sync has not fetched (remote-projects.md § 7.3).
// The sidebar lists them dimmed and quick-open finds them; opening one fetches it
// first. Immutable once built, so one snapshot is shared by the file index and the
// directory tree of a project, and by the thread that built it.
class PendingPaths {
 public:
  struct Child {
    std::string_view name;  // a view into one of files()
    bool is_directory = false;
  };

  // `files`: relative generic ('/'-separated) paths, in any order.
  explicit PendingPaths(std::vector<std::string> files);
  // Children hold views into files_' buffers: never moved or copied.
  PendingPaths(const PendingPaths&) = delete;
  PendingPaths& operator=(const PendingPaths&) = delete;

  // Sorted (bytewise) and unique.
  const std::vector<std::string>& files() const { return files_; }
  bool empty() const { return files_.empty(); }
  // The pending entries directly in `directory` (relative generic, "" = the
  // root), each once, in no particular order; directories are the ones a pending
  // file implies.
  std::span<const Child> ChildrenOf(std::string_view directory) const;

 private:
  std::vector<std::string> files_;
  std::unordered_map<std::string, std::vector<Child>, util::TransparentStringHash, std::equal_to<>>
      children_;
};

using SharedPendingPaths = std::shared_ptr<const PendingPaths>;

}  // namespace microide::project
