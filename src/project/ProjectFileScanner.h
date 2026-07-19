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

// `follow_out_of_root_symlinks` mirrors the `project.follow_out_of_root_symlinks`
// user setting: false (default) refuses directory symlinks whose target escapes
// the project root (DoS/traversal guard); true follows them (cycle-guarded).
// `exclude_globs` are user/project-configured ignore patterns folded in alongside
// the built-in defaults (VCS metadata, dependency/cache, build-output dirs).
//
// `out_incomplete` (optional): set to true when the walk stopped early because it
// hit the entry budget or the max tree-walk depth, so the returned list is only a
// PREFIX of the project tree and must never be presented as an authoritative
// "these are all the files" set (TD-2026-07-17-008/033). Always assigned (false
// when the scan completed) when non-null. `entry_budget` overrides the traversal
// entry cap; it exists so tests can force truncation without materializing a
// 50k-entry tree.
std::vector<std::filesystem::path> CollectProjectFiles(
    const std::filesystem::path& root,
    ProjectFileScanMode mode = ProjectFileScanMode::ExcludeHidden,
    bool follow_out_of_root_symlinks = false,
    const std::vector<std::string>& exclude_globs = {},
    bool* out_incomplete = nullptr,
    std::size_t entry_budget = platform::kTreeTraversalEntryBudget);

}  // namespace microide::project
