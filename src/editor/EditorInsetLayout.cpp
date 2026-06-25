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
  if (!want_surfaces && !want_lenses) {
    return;
  }
  const std::size_t scroll = viewport.scroll_line();
  const std::size_t visual_count = viewport.visual_line_count();
  const std::size_t window_end = std::min(visual_count, scroll + visible_rows);

  // Below: inline plugin-surface insets anchored at a line in the window.
  if (want_surfaces) {
    const std::span<const AnchoredSurface> anchored =
        surface_store.AnchoredSurfacesForPath(viewport.path());
    for (const AnchoredSurface& anchor : anchored) {
      if (anchor.content == nullptr || !anchor.content->has_body()) {
        continue;
      }
      const std::size_t visual_row = viewport.VisualRowForLine(anchor.line);
      if (visual_row >= visual_count || visual_row < scroll || visual_row >= scroll + visible_rows) {
        continue;
      }
      const float height = std::clamp(anchor.content->intrinsic_height, 1.0f, 1024.0f);
      out_gaps.push_back(
          RowGap{static_cast<std::uint32_t>(visual_row), height, RowGapPlacement::Below});
      out_contents.push_back(RowGapContent{.surface = anchor.content});
    }
  }

  // Above: a fixed-height code-lens strip over the first visual row of each
  // visible line that carries a lens. Pointers index the store's sorted lens
  // vector, valid until the next store mutation (same contract as surfaces).
  if (want_lenses) {
    const FileDecorations* decorations = decoration_store.FindByPath(viewport.path());
    if (decorations != nullptr) {
      for (std::size_t visual_row = scroll; visual_row < window_end; ++visual_row) {
        const std::size_t line = viewport.VisualRowLineIndex(visual_row);
        if (viewport.VisualRowForLine(line) != visual_row) {
          continue;  // only the line's first visual row hosts the strip
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
    // A row hosts at most one Above and one Below gap; the strip y fell into tells
    // them apart (Above sits over the text band, Below under it).
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

}  // namespace microide::editor
