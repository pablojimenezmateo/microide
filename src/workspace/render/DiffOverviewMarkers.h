#pragma once

#include <cstdint>

#include <SDL3/SDL.h>

#include "render/Theme.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

// The overview markers of a compare's or a merge's scrollbar lane, cached on the
// tab and pre-mapped to `inner_lane`. Built at frame prep, which damages the lane
// when they change (a result edit that moves a conflict, a side accepted), and
// drawn as they are by paint.
void EnsureCompareOverviewMarkers(const render::Theme& theme,
                                  const SDL_FRect& inner_lane,
                                  std::uint64_t theme_token,
                                  CompareTabState& compare_tab);
void EnsureMergeOverviewMarkers(const render::Theme& theme,
                                const SDL_FRect& inner_lane,
                                std::size_t total_rows,
                                std::uint64_t theme_token,
                                MergeTabState& merge_tab);

}  // namespace microide::workspace
