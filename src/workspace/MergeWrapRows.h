#pragma once

#include <cstddef>
#include <span>

#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

// ---- merge surface row space (TD-2026-08-13-200) ----------------------------
//
// The merge surface stacks three panes that share one scroll offset. With wrap
// off that offset is a document line index and every pane draws its line N at
// row N. With `editor.wrap` on it is an ON-SCREEN row index: the two read-only
// source panes wrap through `merge_tab.wrap_layout` (incoming = left, current =
// right, both at the shared pane width, padded to a common row budget so they
// stay aligned with each other), and the editable result pane wraps through its
// own viewport, whose `scroll_line()` is already a visual row.
//
// Everything downstream — conflict bands, accept buttons, hover classification,
// the overview lane — is expressed in conflict LINE numbers. Rather than teach
// each of those about wrapping, `MergeVisualConflicts` hands back the conflict
// list with every line field replaced by its on-screen row, so the geometry code
// is unchanged and correct in both modes. Conflict indices are preserved, so a
// hit test against the projected list still names the real conflict.

// Rebuild the source-pane wrap table if a key moved. Called from the merge layout
// pass, the one place that knows the pane width.
void EnsureMergeWrapLayout(const MergeTabState& merge_tab,
                           bool soft_wrap,
                           std::size_t pane_columns);

// Rows the two source panes occupy together (they share a padded row budget).
std::size_t MergeSourceVisualRowCount(const MergeTabState& merge_tab);
// Rows the whole surface scrolls through: the taller of the source panes and the
// result pane, which wrap independently.
std::size_t MergeTotalVisualRowCount(const MergeTabState& merge_tab);

std::size_t MergeSourceLineForVisualRow(const MergeTabState& merge_tab, std::size_t visual_row);
std::size_t MergeSourceVisualRowForLine(const MergeTabState& merge_tab, std::size_t line);
std::size_t MergeResultLineForVisualRow(const MergeTabState& merge_tab, std::size_t visual_row);
std::size_t MergeResultVisualRowForLine(const MergeTabState& merge_tab, std::size_t line);

// The conflict list in on-screen row space. Returns `merge_tab.conflicts`
// unchanged (no copy) while wrap is off.
std::span<const MergeTrackedConflict> MergeVisualConflicts(const MergeTabState& merge_tab);

}  // namespace microide::workspace
