#pragma once

#include <filesystem>
#include <unordered_map>

#include "project/DirectoryTree.h"

namespace microide::project {

std::unordered_map<std::string, GitFileStatus> CollectGitStatuses(
    const std::filesystem::path& root);

}  // namespace microide::project
