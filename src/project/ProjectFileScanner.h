#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace microide::project {

enum class ProjectFileScanMode {
  ExcludeHidden,
  IncludeHidden,
};

// `follow_out_of_root_symlinks` mirrors the `project.follow_out_of_root_symlinks`
// user setting: false (default) refuses directory symlinks whose target escapes
// the project root (DoS/traversal guard); true follows them (cycle-guarded).
// `exclude_globs` are user/project-configured ignore patterns folded in alongside
// the built-in defaults (VCS metadata, dependency/cache, build-output dirs).
std::vector<std::filesystem::path> CollectProjectFiles(
    const std::filesystem::path& root,
    ProjectFileScanMode mode = ProjectFileScanMode::ExcludeHidden,
    bool follow_out_of_root_symlinks = false,
    const std::vector<std::string>& exclude_globs = {});

}  // namespace microide::project
