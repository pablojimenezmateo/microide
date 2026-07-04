#include "workspace/MergeResultValidation.h"

#include <fstream>

#include "util/GitConflictMarkers.h"
#include "util/TextFileIO.h"

namespace microide::workspace {

std::optional<std::uint64_t> FileModificationTick(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    return std::nullopt;
  }
  const auto tick = std::filesystem::last_write_time(path, error);
  if (error) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(tick.time_since_epoch().count());
}

bool MergeResultContainsConflictMarkers(std::string_view text) {
  return util::ContainsCompleteConflictMarkers(text);
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

std::size_t CountResolvedMergeConflicts(std::span<const MergeTrackedConflict> conflicts) {
  std::size_t resolved = 0;
  for (const MergeTrackedConflict& conflict : conflicts) {
    if (conflict.valid && conflict.resolved) {
      ++resolved;
    }
  }
  return resolved;
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
  const std::string serialized =
      util::SerializeLines(merge_tab.result_viewport.lines().Snapshot(), merge_tab.result_line_ending);
  if (util::ContainsCompleteConflictMarkers(serialized)) {
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

  const std::string serialized =
      util::SerializeLines(merge_tab.result_viewport.lines().Snapshot(), merge_tab.result_line_ending);
  if (util::ContainsCompleteConflictMarkers(serialized) && !request.allow_conflict_marker_override) {
    const auto marker_line = util::FirstConflictMarkerLine(merge_tab.result_viewport.lines().Snapshot());
    return MergeValidationResult{
        .ok = false,
        .issue = MergeValidationIssue::ConflictMarkers,
        .message = "Conflict markers remain in the merge result.",
        .marker_line = marker_line,
    };
  }

  const bool output_exists =
      !merge_tab.output_path.empty() && std::filesystem::exists(merge_tab.output_path);
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
    if (const auto disk_tick = FileModificationTick(merge_tab.output_path);
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
