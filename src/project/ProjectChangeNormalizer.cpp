#include "project/ProjectChangeNormalizer.h"

#include "util/PathMatch.h"

namespace microide::project {

ProjectChangeBatch NormalizeIndexUpdateBatch(const std::filesystem::path& project_root,
                                             const platform::IndexUpdateBatch& batch) {
  ProjectChangeBatch normalized;
  if (project_root.empty()) {
    return normalized;
  }
  // The coarse "something under the root moved" bit. It is the ONLY thing an
  // initial batch contributes here: is_initial replaces the index wholesale, so
  // its per-file changes describe the whole tree rather than a delta and must not
  // be replayed as external-change events for every file in the project.
  normalized.tree_rescan_requested = batch.tree_structure_changed;
  if (batch.is_initial) {
    return normalized;
  }

  normalized.file_changes.reserve(batch.changes.size());
  // Both `lexically_normal()` calls below used to run per change, at ~12
  // allocations each whether or not they changed anything (TD-2026-08-10-174) —
  // and this runs in the watcher callback for every incremental batch, so a build
  // or a checkout writing into the tree pays it per file. The batch's paths come
  // out of the walk already normal; the root is normalized once, here; and a
  // relative normal path appended to a normal root is normal, so the join needs no
  // second pass.
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

}  // namespace microide::project
