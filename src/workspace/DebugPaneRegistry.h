#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

// One selectable surface in the right-side debug pane's mode-row button switcher.
// Mirrors SidebarViewSpec but is a fixed, debug-owned set with no plugin/policy
// plumbing (nothing contributes right-pane surfaces today).
struct DebugPaneSurfaceSpec {
  std::string_view id;
  std::string_view label;
  DebugPaneMode mode = DebugPaneMode::CallStack;
};

std::span<const DebugPaneSurfaceSpec> BuiltinDebugPaneSurfaceSpecs();
const DebugPaneSurfaceSpec* FindDebugPaneSurface(std::string_view id);
const DebugPaneSurfaceSpec* FindDebugPaneSurface(DebugPaneMode mode);

// One clickable tab in the debug pane's mode-row button switcher. `id`/`label`
// point at the static registry storage, so copies are allocation-free.
struct DebugPaneModeTab {
  std::string_view id;
  std::string_view label;
  DebugPaneMode mode = DebugPaneMode::CallStack;
  SDL_FRect rect{};
};

// Geometry of the debug pane's header mode-row: the four fixed surface tabs. No
// overflow (the set is fixed and small). Collapses to icon-only cells when the
// labelled row doesn't fit. Built without heap allocation so it can be recomputed
// on the render / hit-test paths.
struct DebugPaneModeRowLayout {
  std::array<DebugPaneModeTab, 4> tabs{};
  int tab_count = 0;
  bool icon_only = false;
  SDL_FRect row_rect{};
};

// Shared row-geometry constants so the render TU (DebugPaneRender.cpp), the click
// coordinator, and the cursor path agree on indentation and cannot drift. Value-tree
// rows (Variables / Watch) indent by one step per depth, then reserve a disclosure
// slot before the name; breakpoint rows sit one fixed indent in from the section.
inline constexpr float kDebugPaneTreeIndentStep = 14.0f;       // per-depth value-tree indent
inline constexpr float kDebugPaneTreeDisclosureSlot = 14.0f;   // disclosure -> name gap
inline constexpr float kDebugPaneBreakpointIndent = 16.0f;     // breakpoint row indent

// Result of mapping a point into the debug pane's scrollable row list.
struct DebugPaneRowHit {
  int row_index = -1;     // absolute model row, or -1 when no row was hit
  bool in_content = false;  // the point fell inside the content rect (even if no row)
};

// Map a point to an absolute debug-pane row index. Unlike BottomPanelLineIndexAtY,
// the top text inset (between content_rect.y and text_y) folds into the first visible
// row instead of being a dead-zone that rejects the click. Returns row_index == -1
// when the point is outside content_rect or below the last populated row. Single-
// sourced for the render, click, and hover-cursor paths so they never disagree.
DebugPaneRowHit DebugPaneRowAtPoint(const SDL_FRect& content_rect, float text_y, float line_height,
                                    int visible_rows, int vertical_scroll, std::size_t line_count,
                                    float x, float y);

}  // namespace microide::workspace
