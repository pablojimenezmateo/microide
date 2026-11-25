#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "platform/Filesystem.h"

namespace microide::project {

enum class ProjectFileScanMode {
  ExcludeHidden,
  IncludeHidden,
};

struct ProjectFileScanStatus {
  bool truncated_by_budget = false;
  bool truncated_by_depth = false;
  bool permission_limited = false;
  bool error = false;

  bool incomplete() const {
    return truncated_by_budget || truncated_by_depth || permission_limited || error;
  }
  bool operator==(const ProjectFileScanStatus&) const = default;
};

std::vector<std::filesystem::path> CollectProjectFiles(
    const std::filesystem::path& root,
    ProjectFileScanMode mode = ProjectFileScanMode::ExcludeHidden,
    bool follow_out_of_root_symlinks = false,
    const std::vector<std::string>& exclude_globs = {},
    ProjectFileScanStatus* out_status = nullptr,
    std::size_t entry_budget = platform::kTreeTraversalEntryBudget);

}
