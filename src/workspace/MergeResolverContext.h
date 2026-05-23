#pragma once

#include <filesystem>
#include <string>

#include "compare/MergeConflictKind.h"
#include "project/GitRepositoryState.h"
#include "workspace/MergeResultValidation.h"
#include "workspace/WorkspaceTabState.h"

namespace microide::workspace {

struct MergeResolverLabels {
  std::string incoming_label;
  std::string current_label;
  std::string result_label;
  std::string base_label;
};

struct MergeResolverStatus {
  std::size_t selected_conflict_index = 0;
  std::size_t total_conflicts = 0;
  std::size_t remaining_conflicts = 0;
  std::size_t remaining_files = 0;
  MergeResultState result_state = MergeResultState::Saved;
  std::string result_state_label;
  std::string progress_label;
};

MergeResolverLabels BuildMergeResolverLabels(const std::filesystem::path& project_root,
                                             const std::filesystem::path& output_path,
                                             const project::GitRepositoryState& repository_state);
MergeResolverStatus BuildMergeResolverStatus(const MergeTabState& merge_tab,
                                             std::size_t remaining_conflicted_files);
std::optional<project::GitRepositoryEntry> FindConflictRepositoryEntry(
    const project::GitRepositoryState& repository_state,
    const std::filesystem::path& relative_path);

}  // namespace microide::workspace
