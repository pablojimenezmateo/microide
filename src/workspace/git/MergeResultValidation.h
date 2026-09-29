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

// True when the merge output on disk differs from the text the resolver last wrote
// there.
//
// The modification tick is the cheap first test and NOT the last word: a `touch`, a
// tool that rewrote the output with identical bytes, and a `git checkout` restoring
// it all move the tick and change nothing. Confirming against
// `persisted_output_baseline` — the exact text the resolver wrote — is a byte
// comparison, and it only runs when the tick already says something moved.
//
// Every site that decides the result went stale calls THIS, rather than comparing
// ticks itself. That is the whole point: the confirmation first lived only inside
// ValidateMergeResult, while the two watcher paths that actually SET
// `external_result_stale` kept their own tick compares — so a touch still un-marked
// a completed resolution and the guard never ran. (TD-2026-09-29-306.)
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

// Whether a resolved merge result should leave a file on disk. For an ordinary
// conflict the answer is always yes. For a modify/delete-class conflict
// (requires_existence_choice — DeletedByUs/Them, RenameDelete) the user expresses
// "accept the deletion" by reducing the result to empty content, so an empty
// serialized result means the file should NOT exist. This is derived from the
// serialized content, NOT from result_viewport.lines().empty(): a text buffer is
// normalized to one empty line, so lines().empty() is always false and can never
// signal a delete resolution.
bool ResolvedResultShouldExist(const MergeTabState& merge_tab);

bool MergeResultContainsConflictMarkers(std::string_view text);
MergeResultState ComputeMergeResultState(const MergeTabState& merge_tab,
                                         const compare::MergeFileConflictMetadata& metadata);
std::size_t CountRemainingMergeConflicts(std::span<const MergeTrackedConflict> conflicts);
MergeValidationResult ValidateMergeResult(const MergeValidationRequest& request);

// ValidateMergeResult's external-modification check reads the result file's
// last-write tick via util::FileModificationTick (util/TextFileIO.h); callers that
// write the result file (e.g. Mark Resolved's save) must refresh
// merge_tab.disk_result_tick with the SAME function so their own write is not
// mistaken for an external change.

}  // namespace microide::workspace
