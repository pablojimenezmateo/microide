#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <optional>
#include <string>
#include <string_view>

#include "compare/MergeConflictKind.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

bool MergeResultChangedOnDisk(const MergeTabState& merge_tab);

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

bool ResolvedResultShouldExist(const MergeTabState& merge_tab);

bool MergeResultContainsConflictMarkers(std::string_view text);
MergeResultState ComputeMergeResultState(const MergeTabState& merge_tab,
                                         const compare::MergeFileConflictMetadata& metadata);
std::size_t CountRemainingMergeConflicts(std::span<const MergeTrackedConflict> conflicts);
MergeValidationResult ValidateMergeResult(const MergeValidationRequest& request);
}
