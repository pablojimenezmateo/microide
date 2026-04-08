#pragma once

#include <filesystem>
#include <vector>

namespace microide::project {

enum class ProjectFileScanMode {
  ExcludeHidden,
  IncludeHidden,
};

std::vector<std::filesystem::path> CollectProjectFiles(
    const std::filesystem::path& root,
    ProjectFileScanMode mode = ProjectFileScanMode::ExcludeHidden);

}  // namespace microide::project
