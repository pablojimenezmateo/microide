#pragma once

#include <cstddef>

#include "editor/TextLayout.h"
#include "workspace/WorkspaceTabState.h"

namespace microide::workspace {

void PrepareCompareVisibleLayoutsForWindow(CompareTabState& compare_tab,
                                           std::size_t visible_start_row,
                                           std::size_t visible_end_row,
                                           std::size_t left_visible_columns,
                                           std::size_t right_visible_columns);

const editor::LayoutLine* CompareVisibleLayoutForRow(const CompareTabState& compare_tab,
                                                     std::size_t model_row,
                                                     bool right_side,
                                                     std::size_t visible_columns);

}  // namespace microide::workspace
