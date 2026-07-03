#pragma once

#include <filesystem>
#include <vector>

namespace microide::project {

enum class ProjectFileScanMode {
  ExcludeHidden,
  IncludeHidden,
};

// `follow_out_of_root_symlinks` mirrors the `project.follow_out_of_root_symlinks`
// user setting: false (default) refuses directory symlinks whose target escapes
// the project root (DoS/traversal guard); true follows them (cycle-guarded).
std::vector<std::filesystem::path> CollectProjectFiles(
    const std::filesystem::path& root,
    ProjectFileScanMode mode = ProjectFileScanMode::ExcludeHidden,
    bool follow_out_of_root_symlinks = false);

}  // namespace microide::project
