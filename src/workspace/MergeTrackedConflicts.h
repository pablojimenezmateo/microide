#pragma once

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
std::vector<MergeTrackedConflict> BuildMergeTrackedConflictsForResult(
    compare::MergeModel& model, const std::vector<std::string>& result_lines,
    std::span<const std::vector<std::string>> conflict_line_hints = {},
    std::span<const compare::MergeChoice> choice_hints = {}, std::span<const std::uint8_t> resolved_hints = {});

std::optional<std::size_t> FindMergeTrackedConflictAtSourceLine(const MergeTabState& merge_tab, std::size_t line,
                                                                bool incoming);
std::optional<std::size_t> FindMergeTrackedConflictAtResultLine(const MergeTabState& merge_tab, std::size_t line);

}
