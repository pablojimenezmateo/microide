#pragma once

#include <cstddef>
#include <span>

#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

void EnsureMergeWrapLayout(const MergeTabState& merge_tab,
                           bool soft_wrap,
                           std::size_t pane_columns);

std::size_t MergeSourceVisualRowCount(const MergeTabState& merge_tab);
std::size_t MergeTotalVisualRowCount(const MergeTabState& merge_tab);

std::size_t MergeSourceVisualRowForLine(const MergeTabState& merge_tab, std::size_t line);
std::size_t MergeResultVisualRowForLine(const MergeTabState& merge_tab, std::size_t line);

std::span<const MergeTrackedConflict> MergeVisualConflicts(const MergeTabState& merge_tab);

}
