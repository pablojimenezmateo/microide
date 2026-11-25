#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

struct DebugPaneSurfaceSpec {
  std::string_view id;
  std::string_view label;
  DebugPaneMode mode = DebugPaneMode::CallStack;
};

std::span<const DebugPaneSurfaceSpec> BuiltinDebugPaneSurfaceSpecs();
const DebugPaneSurfaceSpec* FindDebugPaneSurface(std::string_view id);
const DebugPaneSurfaceSpec* FindDebugPaneSurface(DebugPaneMode mode);

struct DebugPaneModeTab {
  std::string_view id;
  std::string_view label;
  DebugPaneMode mode = DebugPaneMode::CallStack;
  SDL_FRect rect{};
};

struct DebugPaneModeRowLayout {
  std::array<DebugPaneModeTab, 4> tabs{};
  int tab_count = 0;
  bool icon_only = false;
  SDL_FRect row_rect{};
};

inline constexpr float kDebugPaneTreeIndentStep = 14.0f;
inline constexpr float kDebugPaneTreeDisclosureSlot = 14.0f;
inline constexpr float kDebugPaneBreakpointIndent = 16.0f;

struct DebugPaneRowHit {
  int row_index = -1;
  bool in_content = false;
};

inline constexpr float kDebugPaneRowBandLift = 1.0f;
[[nodiscard]] inline float DebugPaneRowBandTop(float text_y, float line_height, int visible_row) {
  return text_y - kDebugPaneRowBandLift + static_cast<float>(visible_row) * line_height;
}

DebugPaneRowHit DebugPaneRowAtPoint(const SDL_FRect& content_rect, float text_y, float line_height,
                                    int visible_rows, int vertical_scroll, std::size_t line_count,
                                    float x, float y);

}
