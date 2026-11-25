#include "editor/EditorInsetLayout.h"

#include <algorithm>
#include <vector>

#include "editor/PluginDecorationStore.h"
#include "editor/PluginSurfaceStore.h"
#include "editor/TextViewport.h"

namespace microide::editor {

void BuildRowGapsForWindow(const PluginSurfaceStore& surface_store,
                           const PluginDecorationStore& decoration_store,
                           const TextViewport& viewport,
                           std::size_t visible_rows,
                           const InsetGapOptions& options,
                           std::vector<RowGap>& out_gaps,
                           std::vector<RowGapContent>& out_contents) {
  out_gaps.clear();
  out_contents.clear();
  if (viewport.is_placeholder() || viewport.path().empty()) {
    return;
  }
  const bool want_surfaces = options.inline_surfaces;
  const bool want_lenses = options.code_lens_above && options.code_lens_height > 0.0f;
  const bool want_ghost = options.ghost_height > 0.0f;
  if (!want_surfaces && !want_lenses && !want_ghost) {
    return;
  }
  const std::size_t scroll = viewport.scroll_line();
  const std::size_t visual_count = viewport.visual_line_count();
  const std::size_t window_end = std::min(visual_count, scroll + visible_rows);

  if (want_surfaces && surface_store.has_anchored()) {
    const std::span<const AnchoredSurface> anchored =
        surface_store.AnchoredSurfacesForPathKey(viewport.path_key());
    for (const AnchoredSurface& anchor : anchored) {
      if (anchor.content == nullptr || !anchor.content->has_body()) {
        continue;
      }
      const std::size_t visual_row = viewport.VisualRowForLine(anchor.line);
      if (!VisualRowInWindow(visual_row, scroll, visible_rows, visual_count)) {
        continue;
      }
      const float height = std::clamp(anchor.content->intrinsic_height, 1.0f, 1024.0f);
      out_gaps.push_back(
          RowGap{static_cast<std::uint32_t>(visual_row), height, RowGapPlacement::Below});
      out_contents.push_back(RowGapContent{.surface = anchor.content});
    }
  }

  if (want_lenses) {
    const FileDecorations* decorations = decoration_store.FindByPathKey(viewport.path_key());
    if (decorations != nullptr) {
      for (std::size_t visual_row = scroll; visual_row < window_end; ++visual_row) {
        const std::size_t line = viewport.VisualRowLineIndex(visual_row);
        if (viewport.VisualRowForLine(line) != visual_row) {
          continue;
        }
        const std::span<const CodeLensDecoration> lenses =
            decorations->CodeLensesForLine(static_cast<std::uint32_t>(line));
        if (lenses.empty()) {
          continue;
        }
        out_gaps.push_back(RowGap{static_cast<std::uint32_t>(visual_row), options.code_lens_height,
                                  RowGapPlacement::Above});
        out_contents.push_back(RowGapContent{.code_lens = &lenses.front()});
      }
    }
  }

  if (want_ghost) {
    const std::size_t visual_row = viewport.VisualRowForLine(options.ghost_anchor_line);
    if (VisualRowInWindow(visual_row, scroll, visible_rows, visual_count)) {
      out_gaps.push_back(
          RowGap{static_cast<std::uint32_t>(visual_row), options.ghost_height,
                 RowGapPlacement::Below});
      out_contents.push_back(RowGapContent{.ghost_text = options.ghost_content});
    }
  }
}

InsetClickResult ResolveInsetClick(const PluginSurfaceStore& surface_store,
                                   const PluginDecorationStore& decoration_store,
                                   const TextViewport& viewport, float first_line_y,
                                   float line_height, std::size_t visible_rows, float y,
                                   const InsetGapOptions& options) {
  thread_local std::vector<RowGap> gaps;
  thread_local std::vector<RowGapContent> contents;
  BuildRowGapsForWindow(surface_store, decoration_store, viewport, visible_rows, options, gaps,
                        contents);
  const EditorRowYLayout layout(first_line_y, line_height,
                                static_cast<std::uint32_t>(viewport.scroll_line()), gaps);
  InsetClickResult result{layout.HitTest(y, visible_rows), {}};
  if (result.hit.in_gap) {
    const std::uint32_t target =
        static_cast<std::uint32_t>(viewport.scroll_line() + result.hit.row);
    const bool above_hit = y < layout.RowTop(result.hit.row);
    const RowGapPlacement want = above_hit ? RowGapPlacement::Above : RowGapPlacement::Below;
    for (std::size_t i = 0; i < gaps.size(); ++i) {
      if (gaps[i].visual_row == target && gaps[i].placement == want) {
        result.gap_content = contents[i];
        break;
      }
    }
  }
  return result;
}

}
