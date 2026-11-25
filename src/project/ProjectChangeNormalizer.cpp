#include "project/ProjectChangeNormalizer.h"

#include "util/PathMatch.h"

namespace microide::project {

ProjectChangeBatch NormalizeIndexUpdateBatch(const std::filesystem::path& project_root,
                                             const platform::IndexUpdateBatch& batch) {
  ProjectChangeBatch normalized;
  if (project_root.empty()) {
    return normalized;
  }
  normalized.tree_rescan_requested = batch.tree_structure_changed;
  if (batch.is_initial) {
    return normalized;
  }

  normalized.file_changes.reserve(batch.changes.size());
  std::filesystem::path root_scratch;
  const std::filesystem::path& normalized_root =
      util::NormalizedPathView(project_root, root_scratch);
  std::filesystem::path relative_scratch;
  for (const auto& change : batch.changes) {
    const std::filesystem::path& relative_path =
        util::NormalizedPathView(change.entry.relative_path, relative_scratch);
    if (relative_path.empty()) {
      continue;
    }

    ProjectFileChange file_change{
        .relative_path = relative_path,
        .absolute_path = normalized_root / relative_path,
    };
    switch (change.kind) {
      case platform::IndexUpdateBatch::Kind::CreatedOrModified:
        file_change.kind = ProjectFileChangeKind::Modified;
        break;
      case platform::IndexUpdateBatch::Kind::Deleted:
        file_change.kind = ProjectFileChangeKind::Deleted;
        break;
    }
    normalized.file_changes.push_back(std::move(file_change));
  }
  return normalized;
}

ProjectChangeBatch NormalizeOpenFileChanges(const std::filesystem::path& project_root,
                                            const std::vector<std::filesystem::path>& paths) {
  ProjectChangeBatch normalized;
  normalized.file_changes.reserve(paths.size());
  const std::filesystem::path root = project_root.lexically_normal();
  for (const std::filesystem::path& path : paths) {
    ProjectFileChange change;
    change.absolute_path = path.lexically_normal();
    std::error_code error;
    change.kind = std::filesystem::exists(change.absolute_path, error) && !error
                      ? ProjectFileChangeKind::Modified
                      : ProjectFileChangeKind::Deleted;
    std::filesystem::path relative;
    if (!root.empty()) {
      relative = change.absolute_path.lexically_relative(root);
    }
    const bool inside = !relative.empty() && relative.native() != "." &&
                        *relative.begin() != std::filesystem::path("..");
    change.relative_path = inside ? std::move(relative) : change.absolute_path;
    normalized.file_changes.push_back(std::move(change));
  }
  return normalized;
}

}
