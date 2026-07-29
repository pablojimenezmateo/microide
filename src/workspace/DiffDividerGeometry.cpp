#include "workspace/DiffDividerGeometry.h"

#include <algorithm>

#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

// A grab rect of the shared width, centered on the painted divider band whose
// left edge is `divider_left`.
SDL_FRect CenteredGrabRect(const SDL_FRect& editor_surface, float divider_left,
                           float divider_width) {
  const float hit_width = std::max(kDiffDividerHitWidth, divider_width);
  const float center_x = divider_left + divider_width * 0.5f;
  return MakeRect(center_x - hit_width * 0.5f, editor_surface.y, hit_width, editor_surface.h);
}

}  // namespace

SDL_FRect CompareDividerHitRect(const SDL_FRect& editor_surface,
                                const WorkspaceShell::CompareSurfaceLayout& surface) {
  return CenteredGrabRect(editor_surface, surface.center_x, surface.divider_width);
}

std::array<SDL_FRect, 2> MergeDividerHitRects(
    const SDL_FRect& editor_surface, const WorkspaceShell::MergeSurfaceLayout& surface) {
  return {
      CenteredGrabRect(editor_surface, surface.center_x - surface.divider_width,
                       surface.divider_width),
      CenteredGrabRect(editor_surface, surface.right_x - surface.divider_width,
                       surface.divider_width),
  };
}

}  // namespace microide::workspace
