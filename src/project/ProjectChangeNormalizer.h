#pragma once

#include <filesystem>
#include <vector>

#include "platform/FileIndexWatcher.h"
#include "project/ProjectChangeTypes.h"

namespace microide::project {

ProjectChangeBatch NormalizeIndexUpdateBatch(const std::filesystem::path& project_root,
                                             const platform::IndexUpdateBatch& batch);

ProjectChangeBatch NormalizeOpenFileChanges(const std::filesystem::path& project_root,
                                            const std::vector<std::filesystem::path>& paths);

}
