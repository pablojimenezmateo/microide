#include "workspace/MergeWrapRows.h"

#include <algorithm>

namespace microide::workspace {

namespace {

DiffWrapLayout::UnitText MergeWrapUnitText(const MergeTabState& merge_tab, std::size_t unit) {
  DiffWrapLayout::UnitText text;
  if (unit < merge_tab.model.incoming_lines.size()) {
    text.has_left = true;
    text.left = merge_tab.model.incoming_lines[unit];
  }
  if (unit < merge_tab.model.current_lines.size()) {
    text.has_right = true;
    text.right = merge_tab.model.current_lines[unit];
  }
  return text;
}

// End of a half-open LINE range, projected to the first on-screen row past it.
// A range that ends at (or past) the document end maps to the row count, so the
// band covers the last line's final wrapped row rather than stopping short.
std::size_t ProjectRangeEnd(std::size_t end_line,
                            std::size_t unit_count,
                            std::size_t row_count,
                            const MergeTabState& merge_tab,
                            bool source) {
  if (end_line >= unit_count) {
    return row_count;
  }
  return source ? MergeSourceVisualRowForLine(merge_tab, end_line)
                : MergeResultVisualRowForLine(merge_tab, end_line);
}

}  // namespace

void EnsureMergeWrapLayout(const MergeTabState& merge_tab,
                           bool soft_wrap,
                           std::size_t pane_columns) {
  const std::size_t unit_count =
      std::max(merge_tab.model.incoming_lines.size(), merge_tab.model.current_lines.size());
  merge_tab.wrap_layout.Ensure(
      merge_tab.model_revision, soft_wrap, pane_columns, pane_columns,
      merge_tab.result_viewport.tab_size(), unit_count,
      [&merge_tab](std::size_t unit) { return MergeWrapUnitText(merge_tab, unit); });
}

std::size_t MergeSourceVisualRowCount(const MergeTabState& merge_tab) {
  return merge_tab.wrap_layout.RowCount(
      std::max(merge_tab.model.incoming_lines.size(), merge_tab.model.current_lines.size()));
}

std::size_t MergeTotalVisualRowCount(const MergeTabState& merge_tab) {
  const std::size_t result_rows =
      merge_tab.wrap_layout.active()
          ? static_cast<std::size_t>(std::max(0, merge_tab.result_viewport.VisualRowCount()))
          : merge_tab.result_viewport.line_count();
  return std::max({MergeSourceVisualRowCount(merge_tab), result_rows, std::size_t{1}});
}

std::size_t MergeSourceLineForVisualRow(const MergeTabState& merge_tab, std::size_t visual_row) {
  return merge_tab.wrap_layout.UnitForRow(visual_row);
}

std::size_t MergeSourceVisualRowForLine(const MergeTabState& merge_tab, std::size_t line) {
  return merge_tab.wrap_layout.FirstRowForUnit(line);
}

std::size_t MergeResultLineForVisualRow(const MergeTabState& merge_tab, std::size_t visual_row) {
  if (!merge_tab.wrap_layout.active()) {
    return visual_row;
  }
  return merge_tab.result_viewport.VisualRowLineIndex(visual_row);
}

std::size_t MergeResultVisualRowForLine(const MergeTabState& merge_tab, std::size_t line) {
  if (!merge_tab.wrap_layout.active()) {
    return line;
  }
  return merge_tab.result_viewport.VisualRowForLine(line);
}

std::span<const MergeTrackedConflict> MergeVisualConflicts(const MergeTabState& merge_tab) {
  if (!merge_tab.wrap_layout.active()) {
    return std::span<const MergeTrackedConflict>(merge_tab.conflicts);
  }
  // Keyed on everything that can move a projection: the model (which rebuilds the
  // source wrap table and the conflict list), the result buffer, and the two row
  // counts, which is how a pane-width change shows up here.
  const std::size_t source_rows = MergeSourceVisualRowCount(merge_tab);
  const std::size_t result_rows =
      static_cast<std::size_t>(std::max(0, merge_tab.result_viewport.VisualRowCount()));
  const std::uint64_t key = merge_tab.model_revision * 0x9E3779B97F4A7C15ull ^
                            merge_tab.result_viewport.content_revision() ^
                            (static_cast<std::uint64_t>(source_rows) << 20) ^
                            (static_cast<std::uint64_t>(result_rows) << 40);
  if (merge_tab.visual_conflicts_valid && merge_tab.visual_conflicts_key == key &&
      merge_tab.visual_conflicts.size() == merge_tab.conflicts.size()) {
    return std::span<const MergeTrackedConflict>(merge_tab.visual_conflicts);
  }

  const std::size_t source_units =
      std::max(merge_tab.model.incoming_lines.size(), merge_tab.model.current_lines.size());
  const std::size_t result_units = merge_tab.result_viewport.line_count();
  merge_tab.visual_conflicts.assign(merge_tab.conflicts.begin(), merge_tab.conflicts.end());
  for (MergeTrackedConflict& conflict : merge_tab.visual_conflicts) {
    conflict.incoming_start_line =
        MergeSourceVisualRowForLine(merge_tab, conflict.incoming_start_line);
    conflict.incoming_end_line = ProjectRangeEnd(conflict.incoming_end_line, source_units,
                                                 source_rows, merge_tab, /*source=*/true);
    conflict.current_start_line =
        MergeSourceVisualRowForLine(merge_tab, conflict.current_start_line);
    conflict.current_end_line = ProjectRangeEnd(conflict.current_end_line, source_units,
                                                source_rows, merge_tab, /*source=*/true);
    conflict.start_line = MergeResultVisualRowForLine(merge_tab, conflict.start_line);
    conflict.end_line = ProjectRangeEnd(conflict.end_line, result_units, result_rows, merge_tab,
                                        /*source=*/false);
  }
  merge_tab.visual_conflicts_key = key;
  merge_tab.visual_conflicts_valid = true;
  return std::span<const MergeTrackedConflict>(merge_tab.visual_conflicts);
}

}  // namespace microide::workspace
