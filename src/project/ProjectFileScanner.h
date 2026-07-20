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

// Why a scan returned only a prefix of the tree, so callers can surface a specific
// cause instead of a bare "incomplete" (TD-2026-07-17-008/033). A single scan can
// trip more than one condition, so this is a set of independent flags rather than
// an enum. `incomplete()` is the derived "must not be treated as authoritative".
struct ProjectFileScanStatus {
  bool truncated_by_budget = false;  // hit the entry budget (project too large)
  bool truncated_by_depth = false;   // hit kMaxTreeWalkDepth (tree too deep)
  bool permission_limited = false;   // a directory could not be opened (unreadable)
  bool error = false;                // the root itself could not be iterated

  bool incomplete() const {
    return truncated_by_budget || truncated_by_depth || permission_limited || error;
  }
  bool operator==(const ProjectFileScanStatus&) const = default;
};

// `follow_out_of_root_symlinks` mirrors the `project.follow_out_of_root_symlinks`
// user setting: false (default) refuses directory symlinks whose target escapes
// the project root (DoS/traversal guard); true follows them (cycle-guarded).
// `exclude_globs` are user/project-configured ignore patterns folded in alongside
// the built-in defaults (VCS metadata, dependency/cache, build-output dirs).
//
// `out_status` (optional): reports why (if at all) the returned list is only a
// PREFIX of the project tree, so it is never presented as an authoritative "these
// are all the files" set (TD-2026-07-17-008/033). Always assigned (a clean
// all-false status when the scan completed) when non-null. `entry_budget`
// overrides the traversal entry cap; it exists so tests can force truncation
// without materializing a 50k-entry tree.
std::vector<std::filesystem::path> CollectProjectFiles(
    const std::filesystem::path& root,
    ProjectFileScanMode mode = ProjectFileScanMode::ExcludeHidden,
    bool follow_out_of_root_symlinks = false,
    const std::vector<std::string>& exclude_globs = {},
    ProjectFileScanStatus* out_status = nullptr,
    std::size_t entry_budget = platform::kTreeTraversalEntryBudget);

}  // namespace microide::project
