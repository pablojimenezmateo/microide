#pragma once

#include <cstddef>

#include "editor/TextLayout.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

void ResetCompareVisibleLayoutCache(CompareTabState& compare_tab);

void PrepareCompareVisibleLayoutsForWindow(CompareTabState& compare_tab,
                                           std::size_t visual_start_row,
                                           std::size_t visual_end_row,
                                           std::size_t left_visible_columns,
                                           std::size_t right_visible_columns);

const editor::LayoutLine* CompareVisibleLayoutForRow(const CompareTabState& compare_tab,
                                                     std::size_t model_row,
                                                     bool right_side,
                                                     std::size_t row_visual_start,
                                                     std::size_t visible_columns);

}
