#pragma once

#include <filesystem>

#include "platform/FileIndexWatcher.h"
#include "project/ProjectChangeTypes.h"

namespace microide::project {

ProjectChangeBatch NormalizeIndexUpdateBatch(const std::filesystem::path& project_root,
                                             const platform::IndexUpdateBatch& batch);

}  // namespace microide::project
