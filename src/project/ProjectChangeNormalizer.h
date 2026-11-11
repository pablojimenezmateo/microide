#pragma once

#include <filesystem>
#include <vector>

#include "platform/FileIndexWatcher.h"
#include "project/ProjectChangeTypes.h"

namespace microide::project {

ProjectChangeBatch NormalizeIndexUpdateBatch(const std::filesystem::path& project_root,
                                             const platform::IndexUpdateBatch& batch);

// What the open-document watcher reported (platform::OpenFileWatcher), as the
// changes the project's own watcher would have named: Modified when the file is
// there, Deleted when it is not. A path inside the root is keyed by its relative
// path, so the same change reported by both watchers coalesces into one; a path
// outside it is keyed by its absolute path, an entry of its own.
ProjectChangeBatch NormalizeOpenFileChanges(const std::filesystem::path& project_root,
                                            const std::vector<std::filesystem::path>& paths);

}  // namespace microide::project
