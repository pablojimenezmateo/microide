#pragma once

#include <cstdint>
#include <span>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "compare/MergeConflictKind.h"
#include "workspace/WorkspaceTabState.h"

namespace microide::workspace {

enum class MergeResultState {
  Dirty,
  Saved,
  Invalid,
  Resolved,
  Stale,
};

enum class MergeValidationIssue {
  None,
  Unsaved,
  ConflictMarkers,
  ExpectedExistenceMismatch,
  ExternalModification,
  StaleIndexGeneration,
  LineEndingMismatch,
};

struct MergeValidationResult {
  bool ok = false;
  MergeValidationIssue issue = MergeValidationIssue::None;
  std::string message;
  std::optional<std::size_t> marker_line;
};

struct MergeValidationRequest {
  const MergeTabState& merge_tab = {};
  std::filesystem::path project_root;
  std::uint64_t repository_generation = 0;
  bool allow_conflict_marker_override = false;
  bool result_should_exist = true;
};

bool MergeResultContainsConflictMarkers(std::string_view text);
MergeResultState ComputeMergeResultState(const MergeTabState& merge_tab,
                                         const compare::MergeFileConflictMetadata& metadata);
std::size_t CountRemainingMergeConflicts(std::span<const MergeTrackedConflict> conflicts);
std::size_t CountResolvedMergeConflicts(std::span<const MergeTrackedConflict> conflicts);
MergeValidationResult ValidateMergeResult(const MergeValidationRequest& request);

// The file's last-write tick, or nullopt if it does not exist / cannot be read.
// ValidateMergeResult's external-modification check reads this; callers that write
// the result file (e.g. Mark Resolved's save) must refresh merge_tab.disk_result_tick
// with the SAME function so their own write is not mistaken for an external change.
std::optional<std::uint64_t> FileModificationTick(const std::filesystem::path& path);

}  // namespace microide::workspace
