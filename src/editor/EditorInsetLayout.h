#pragma once

#include <cstddef>
#include <vector>

#include "editor/EditorRowYLayout.h"

namespace microide::editor {

class TextViewport;
class PluginSurfaceStore;
class PluginDecorationStore;
struct GhostTextInset;

// What inline insets to resolve into row gaps. Off by default so the common
// (no-inset) editor path produces an empty gap list and stays bit-identical to
// the legacy `first_line_y + row*line_height` mapping.
struct InsetGapOptions {
  bool inline_surfaces = false;   // `plugins.inline_surfaces`: anchored surface insets (Below)
  bool code_lens_above = false;   // `plugins.code_lens_above`: above-line code-lens strips (Above)
  float code_lens_height = 0.0f;  // height of the Above code-lens strip (a text line height)
  // Ghost-text below-caret block (Copilot). Emitted as a single Below gap at
  // `ghost_anchor_line`'s visual row when `ghost_height > 0`. Routed through this
  // one producer so the render and hit-test paths share identical geometry; the
  // content pointer is null on the hit-test path (geometry only) and set on the
  // render path so the gap can be drawn.
  std::size_t ghost_anchor_line = 0;             // 0-based logical line of the caret
  float ghost_height = 0.0f;                     // (below-line count) * line height; 0 => none
  const GhostTextInset* ghost_content = nullptr;
};

// The two inline-inset feature toggles, grouped so the render path threads one
// value instead of a pair of loose bools. The per-call line height (which varies
// as sticky-scroll recomputes metrics) stays a separate argument and combines with
// these into `InsetGapOptions` at the point of use.
struct InsetGapFeatureFlags {
  bool inline_surfaces = false;
  bool code_lens_above = false;
  bool ghost_text = false;
};

// Resolve the inert row gaps for inline insets visible in the window
// [scroll_line, scroll_line + visible_rows): plugin-surface insets (Below) and
// above-line code-lens strips (Above). `out_gaps` and `out_contents` are parallel
// (same index → the content hosted by that gap) and cleared first; they stay
// empty when both options are off, the viewport has no path, or nothing is
// visible — so resolution is O(visible insets) and the no-inset path allocates
// nothing.
//
// This is the single producer of row gaps, called by both the render view-model
// builder and the input hit-test paths so caret/click see exactly the gaps the
// renderer drew.
void BuildRowGapsForWindow(const PluginSurfaceStore& surface_store,
                           const PluginDecorationStore& decoration_store,
                           const TextViewport& viewport,
                           std::size_t visible_rows,
                           const InsetGapOptions& options,
                           std::vector<RowGap>& out_gaps,
                           std::vector<RowGapContent>& out_contents);

// Result of resolving a pointer against the inline-inset geometry: the row hit
// (gap-aware) plus the content of the gap it landed in (null when it hit text).
struct InsetClickResult {
  EditorRowYLayout::HitResult hit;
  RowGapContent gap_content;  // surface/code_lens both null when `hit.in_gap` is false
};

// Convenience for input hit-testing: build the visible window's inset gaps and
// resolve the visible-row offset, in-gap flag, and (when in a gap) the hosted
// content for a pointer at screen-y `y`, using thread_local scratch internally.
// With both options off (or nothing visible) the result is the legacy
// `(y - first_line_y) / line_height` row, so the no-inset input path is unchanged.
InsetClickResult ResolveInsetClick(const PluginSurfaceStore& surface_store,
                                   const PluginDecorationStore& decoration_store,
                                   const TextViewport& viewport, float first_line_y,
                                   float line_height, std::size_t visible_rows, float y,
                                   const InsetGapOptions& options);

}  // namespace microide::editor
