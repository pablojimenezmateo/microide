#include "workspace/git/MergeResultValidation.h"


#include "util/GitConflictMarkers.h"
#include "util/TextFileIO.h"

namespace microide::workspace {

bool MergeResultContainsConflictMarkers(std::string_view text) {
  return util::ContainsCompleteConflictMarkers(text);
}

bool ResolvedResultShouldExist(const MergeTabState& merge_tab) {
  if (!merge_tab.file_conflict.requires_existence_choice) {
    return true;
  }
  // Existence-choice conflict: an empty serialized result means the user accepted
  // the deletion, so the file should not exist. Non-empty content means keep it.
  // Check emptiness against the live LineSpan without serializing/materializing:
  // SerializeLines is empty iff there are no lines or a single empty line (any
  // second line contributes a separator, and a non-empty first line contributes
  // content).
  const auto lines = merge_tab.result_viewport.lines();
  const bool result_empty = lines.size() == 0 || (lines.size() == 1 && lines[0].empty());
  return !result_empty;
}

std::size_t CountRemainingMergeConflicts(std::span<const MergeTrackedConflict> conflicts) {
  std::size_t remaining = 0;
  for (const MergeTrackedConflict& conflict : conflicts) {
    if (conflict.valid && !conflict.resolved) {
      ++remaining;
    }
  }
  return remaining;
}

MergeResultState ComputeMergeResultState(const MergeTabState& merge_tab,
                                         const compare::MergeFileConflictMetadata& metadata) {
  if (merge_tab.index_stale || merge_tab.external_result_stale) {
    return MergeResultState::Stale;
  }
  if (merge_tab.marked_resolved) {
    return MergeResultState::Resolved;
  }
  if (merge_tab.result_viewport.dirty()) {
    return MergeResultState::Dirty;
  }
  if (!metadata.text_hunks_available) {
    return MergeResultState::Invalid;
  }
  // Scan the live buffer directly for conflict markers (no Snapshot/serialize).
  if (util::ScanConflictMarkers(merge_tab.result_viewport.lines()).complete) {
    return MergeResultState::Invalid;
  }
  return MergeResultState::Saved;
}

MergeValidationResult ValidateMergeResult(const MergeValidationRequest& request) {
  const MergeTabState& merge_tab = request.merge_tab;
  if (merge_tab.result_viewport.dirty()) {
    return MergeValidationResult{
        .ok = false,
        .issue = MergeValidationIssue::Unsaved,
        .message = "Save the merge result before marking the file resolved.",
        .marker_line = std::nullopt,
    };
  }

  // One pass over the live buffer yields both "markers still present" and the first
  // marker line — no double Snapshot(), no whole-document serialize (TD-2026-07-17A-009).
  const util::ConflictMarkerScan conflicts =
      util::ScanConflictMarkers(merge_tab.result_viewport.lines());
  if (conflicts.complete && !request.allow_conflict_marker_override) {
    return MergeValidationResult{
        .ok = false,
        .issue = MergeValidationIssue::ConflictMarkers,
        .message = "Conflict markers remain in the merge result.",
        .marker_line = conflicts.first_marker_line,
    };
  }

  std::error_code output_exists_ec;
  const bool output_exists = !merge_tab.output_path.empty() &&
                             std::filesystem::exists(merge_tab.output_path, output_exists_ec) &&
                             !output_exists_ec;
  if (request.result_should_exist != output_exists) {
    return MergeValidationResult{
        .ok = false,
        .issue = MergeValidationIssue::ExpectedExistenceMismatch,
        .message = request.result_should_exist
                       ? "The merge result file must exist before marking resolved."
                       : "Remove the merge result file before marking this delete conflict resolved.",
        .marker_line = std::nullopt,
    };
  }

  if (merge_tab.external_result_stale) {
    return MergeValidationResult{
        .ok = false,
        .issue = MergeValidationIssue::ExternalModification,
        .message = "The merge result changed on disk; refresh the resolver before marking resolved.",
        .marker_line = std::nullopt,
    };
  }

  if (merge_tab.open_index_generation != 0 && merge_tab.open_index_generation != request.repository_generation) {
    return MergeValidationResult{
        .ok = false,
        .issue = MergeValidationIssue::StaleIndexGeneration,
        .message = "Git index conflict state changed; refresh the resolver before marking resolved.",
        .marker_line = std::nullopt,
    };
  }

  if (merge_tab.persisted_output_baseline.has_value()) {
    const auto baseline_ending = util::DetectLineEnding(*merge_tab.persisted_output_baseline);
    if (baseline_ending != merge_tab.result_line_ending) {
      return MergeValidationResult{
          .ok = false,
          .issue = MergeValidationIssue::LineEndingMismatch,
          .message = "Line endings differ from the opened result file.",
          .marker_line = std::nullopt,
      };
    }
  }

  if (!merge_tab.output_path.empty()) {
    if (const auto disk_tick = util::FileModificationTick(merge_tab.output_path);
        disk_tick.has_value() && merge_tab.disk_result_tick.has_value() &&
        *disk_tick != *merge_tab.disk_result_tick) {
      return MergeValidationResult{
          .ok = false,
          .issue = MergeValidationIssue::ExternalModification,
          .message = "The merge result changed on disk; refresh the resolver before marking resolved.",
          .marker_line = std::nullopt,
      };
    }
  }

  return MergeValidationResult{
      .ok = true,
      .message = {},
      .marker_line = std::nullopt,
  };
}

}  // namespace microide::workspace
