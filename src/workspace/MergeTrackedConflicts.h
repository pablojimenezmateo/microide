#pragma once

// A merge tab's conflicts as tracked spans: built from the model's hunks (and
// matched against a result buffer that may already hold choices), and looked up
// by an on-screen source or result line. Pure functions of the tab's state.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "compare/MergeModel.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

std::vector<MergeTrackedConflict> BuildMergeTrackedConflicts(const compare::MergeModel& model);
// `result_lines` may already hold resolved choices; the hints carry what an earlier
// tracking knew (each conflict's lines, choice and resolved flag) across a rebuild.
std::vector<MergeTrackedConflict> BuildMergeTrackedConflictsForResult(
    compare::MergeModel& model, const std::vector<std::string>& result_lines,
    std::span<const std::vector<std::string>> conflict_line_hints = {},
    std::span<const compare::MergeChoice> choice_hints = {}, std::span<const std::uint8_t> resolved_hints = {});

// The conflict on an on-screen source (`incoming` or current) or result row.
std::optional<std::size_t> FindMergeTrackedConflictAtSourceLine(const MergeTabState& merge_tab, std::size_t line,
                                                                bool incoming);
std::optional<std::size_t> FindMergeTrackedConflictAtResultLine(const MergeTabState& merge_tab, std::size_t line);

}  // namespace microide::workspace
