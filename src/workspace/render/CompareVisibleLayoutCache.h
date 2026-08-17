#pragma once

#include <cstddef>

#include "editor/TextLayout.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

// Drop every cached layout without releasing its storage: the slab keeps its
// `LayoutLine` buffers and the next build refills them in place. Call this
// instead of clearing `visible_layout_cache` — the clear freed three heap
// buffers per visible row that the next frame allocated straight back, which on
// the editable pane is every keystroke (TD-2026-08-17-261).
void ResetCompareVisibleLayoutCache(CompareTabState& compare_tab);

// Build the visible-window LayoutLine for every pane row in the on-screen row
// range [visual_start_row, visual_end_row). Rows are on-screen rows, which with
// soft wrap off are presentation rows one-for-one and with it on are wrap
// segments — so a wrapped line contributes one cached layout per segment, keyed
// by that segment's first visual column.
void PrepareCompareVisibleLayoutsForWindow(CompareTabState& compare_tab,
                                           std::size_t visual_start_row,
                                           std::size_t visual_end_row,
                                           std::size_t left_visible_columns,
                                           std::size_t right_visible_columns);

// `row_visual_start` / `visible_columns` must be the same pair the prepare pass
// used for this row: the segment span under wrap, the shared horizontal scroll
// and pane width without it.
const editor::LayoutLine* CompareVisibleLayoutForRow(const CompareTabState& compare_tab,
                                                     std::size_t model_row,
                                                     bool right_side,
                                                     std::size_t row_visual_start,
                                                     std::size_t visible_columns);

}  // namespace microide::workspace
