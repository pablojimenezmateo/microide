#pragma once

#include <cstdint>

#include <SDL3/SDL.h>

#include "render/Theme.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

void EnsureCompareOverviewMarkers(const render::Theme& theme,
                                  const SDL_FRect& inner_lane,
                                  std::uint64_t theme_token,
                                  CompareTabState& compare_tab);
void EnsureMergeOverviewMarkers(const render::Theme& theme,
                                const SDL_FRect& inner_lane,
                                std::size_t total_rows,
                                std::uint64_t theme_token,
                                MergeTabState& merge_tab);

}
