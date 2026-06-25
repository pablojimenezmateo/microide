#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "editor/EditorRowYLayout.h"

namespace microide::editor {

struct SurfaceContent;  // editor/PluginSurfaceStore.h (Phase E inline insets)

struct FoldGutterMark {
  std::size_t line_index = 0;
  std::size_t visual_row_index = 0;
  bool collapsed = false;
};

struct BreakpointGutterMark {
  std::size_t line_index = 0;
  std::size_t visual_row_index = 0;
  bool enabled = true;
  bool verified = false;
  // Phase 9 gutter-dot distinction: a condition or hit-count makes the dot read
  // as conditional (tinted); a log message makes it a logpoint (diamond shape).
  // `is_logpoint` wins when both are set (a logpoint never pauses execution).
  bool has_condition = false;
  bool is_logpoint = false;
};

struct OccurrenceRange {
  std::size_t line_index = 0;
  std::size_t start_column = 0;
  std::size_t end_column = 0;
  bool is_primary_seed = false;
};

/// One whitespace decoration for a clipped visual row fragment (tabs span
/// multiple visual cells across the wrapped-row slice intersecting `[row_visual_start,
/// row_visual_end)`).
struct WhitespaceGlyphRun {
  std::size_t visual_row_index = 0;
  std::size_t row_visual_start = 0;
  std::size_t row_visual_end = 0;
  std::size_t cell_visual_start = 0;
  std::size_t cell_visual_extent = 1;
  bool is_tab_rule = false;
};

struct EditorViewModel {
  std::vector<FoldGutterMark> fold_gutter_marks;
  // Breakpoint dots for visible rows (empty unless the debugger is enabled).
  std::vector<BreakpointGutterMark> breakpoint_gutter_marks;
  // `occurrence_ranges` and `sticky_lines` are views into thread_local builder caches owned by
  // `RenderViewModelBuilder`. They stay valid until the next BuildEditorViewModelInto on the
  // same thread, which matches the render-frame lifetime of this view model. The view-into-cache
  // form avoids the per-frame element copy that the previous owning vectors required.
  std::span<const OccurrenceRange> occurrence_ranges;
  // Logical opener line indices from outer enclosing fold to inner, pinned in the sticky band
  // (top row = outer scope). Empty when sticky scroll is disabled or no enclosing folds apply.
  std::span<const std::size_t> sticky_lines;
  // 0-based buffer line of the debugger's current execution line, set only when
  // a session is stopped on this viewport's file (debugger enabled). Drives the
  // full-width execution-line fill + gutter arrow. Empty in the common case.
  std::optional<std::size_t> execution_line_index;
  // Inert vertical gaps (inline plugin-surface insets, Phase E1), sorted by
  // visual row. Empty unless the `plugins.inline_surfaces` setting is on, so the
  // EditorRowYLayout the renderer builds from this is bit-identical to the legacy
  // `first_line_y + row*line_height` mapping in the common case. `row_gap_contents`
  // is parallel to `row_gaps` (same index) and carries the surface each gap hosts
  // so the workspace draw pass can paint it; EditorViewRenderer ignores it.
  std::span<const RowGap> row_gaps;
  std::span<const SurfaceContent* const> row_gap_contents;
  std::vector<WhitespaceGlyphRun> whitespace_glyph_runs;
  // CSR-style index into `whitespace_glyph_runs`: for visible row `r`, runs are in
  // [whitespace_row_offsets[r], whitespace_row_offsets[r+1]). Size is `visible_rows + 1` whenever
  // whitespace painting is enabled and `visible_rows > 0`. Lets the per-row paint loop iterate
  // only its own runs instead of filtering the flat vector. (Round-2 Finding 2.)
  std::vector<std::size_t> whitespace_row_offsets;
};

}  // namespace microide::editor
