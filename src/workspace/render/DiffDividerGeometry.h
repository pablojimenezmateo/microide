#pragma once

#include <array>

#include <SDL3/SDL.h>

#include "workspace/shell/WorkspaceShell.h"

namespace microide::workspace {

inline constexpr float kDiffDividerHitWidth = 12.0f;

SDL_FRect CompareDividerHitRect(const SDL_FRect& editor_surface, const workspace::CompareSurfaceLayout& surface);

std::array<SDL_FRect, 2> MergeDividerHitRects(const SDL_FRect& editor_surface,
                                              const workspace::MergeSurfaceLayout& surface);

}
