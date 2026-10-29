#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace microide::project {

enum class ProjectFileChangeKind {
  Created,
  Modified,
  Deleted,
};

struct ProjectFileChange {
  ProjectFileChangeKind kind = ProjectFileChangeKind::Modified;
  std::filesystem::path relative_path;
  std::filesystem::path absolute_path;
};

enum class RepositoryChangeKind {
  HeadChanged,
  IndexChanged,
};

struct RepositoryChange {
  RepositoryChangeKind kind = RepositoryChangeKind::IndexChanged;
};

struct ProjectChangeBatch {
  std::vector<ProjectFileChange> file_changes;
  std::vector<RepositoryChange> repository_changes;
  std::uint64_t generation = 0;
  bool tree_rescan_requested = false;
  // The coalescer stopped itemizing: `file_changes` is the first
  // kMaxPendingFileChanges of a flood, not all of it. A path past the cap changed
  // too and is named nowhere, so whatever acts per path (reloading open buffers)
  // has to re-check everything instead.
  bool file_changes_truncated = false;
  // A truncated flood's changes past that cap, which only the language servers
  // hear (didChangeWatchedFiles): nothing acts on them per path -- the truncated
  // apply re-checks every open buffer instead -- but a server has no other way to
  // learn that a file it indexed changed, and answered a `git checkout` of a
  // large tree from a partly stale index (TD-2026-10-29-422 item 1). Bounded too
  // (ProjectChangeCoalescer); past that bound a server still misses changes.
  std::vector<ProjectFileChange> unitemized_file_changes;
};

}  // namespace microide::project
