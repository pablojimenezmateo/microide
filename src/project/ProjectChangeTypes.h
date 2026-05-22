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
};

}  // namespace microide::project
