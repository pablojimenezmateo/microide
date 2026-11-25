#pragma once

#include <cstddef>
#include <vector>

#include "editor/EditorRowYLayout.h"

namespace microide::editor {

class TextViewport;
class PluginSurfaceStore;
class PluginDecorationStore;
struct GhostTextInset;

[[nodiscard]] inline bool VisualRowInWindow(std::size_t visual_row, std::size_t scroll,
                                            std::size_t visible_rows, std::size_t visual_count) {
  return visual_row < visual_count && visual_row >= scroll && visual_row < scroll + visible_rows;
}

struct InsetGapOptions {
  bool inline_surfaces = false;
  bool code_lens_above = false;
  float code_lens_height = 0.0f;
  std::size_t ghost_anchor_line = 0;
  float ghost_height = 0.0f;
  const GhostTextInset* ghost_content = nullptr;
};

struct InsetGapFeatureFlags {
  bool inline_surfaces = false;
  bool code_lens_above = false;
  bool ghost_text = false;
};

void BuildRowGapsForWindow(const PluginSurfaceStore& surface_store,
                           const PluginDecorationStore& decoration_store,
                           const TextViewport& viewport,
                           std::size_t visible_rows,
                           const InsetGapOptions& options,
                           std::vector<RowGap>& out_gaps,
                           std::vector<RowGapContent>& out_contents);

struct InsetClickResult {
  EditorRowYLayout::HitResult hit;
  RowGapContent gap_content;
};

InsetClickResult ResolveInsetClick(const PluginSurfaceStore& surface_store,
                                   const PluginDecorationStore& decoration_store,
                                   const TextViewport& viewport, float first_line_y,
                                   float line_height, std::size_t visible_rows, float y,
                                   const InsetGapOptions& options);

}
