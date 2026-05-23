#include "project/ProjectChangeNormalizer.h"

namespace microide::project {

ProjectChangeBatch NormalizeIndexUpdateBatch(const std::filesystem::path& project_root,
                                             const platform::IndexUpdateBatch& batch) {
  ProjectChangeBatch normalized;
  if (batch.is_initial || project_root.empty()) {
    return normalized;
  }

  normalized.file_changes.reserve(batch.changes.size());
  for (const auto& change : batch.changes) {
    const std::filesystem::path relative_path = change.entry.relative_path.lexically_normal();
    if (relative_path.empty()) {
      continue;
    }

    ProjectFileChange file_change{
        .relative_path = relative_path,
        .absolute_path = (project_root / relative_path).lexically_normal(),
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

}  // namespace microide::project
