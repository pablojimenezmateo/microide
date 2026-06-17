#pragma once

#include <SDL3/SDL.h>

#include <array>
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

}  // namespace microide::workspace
