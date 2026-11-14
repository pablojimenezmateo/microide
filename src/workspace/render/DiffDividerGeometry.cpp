#include "workspace/render/DiffDividerGeometry.h"

#include <algorithm>

#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

// A grab rect of the shared width, centered on the painted divider band whose
// left edge is `divider_left`, and clipped to the pane it is in.
SDL_FRect CenteredGrabRect(const SDL_FRect& editor_surface, float divider_left,
                           float divider_width) {
  const float hit_width = std::max(kDiffDividerHitWidth, divider_width);
  const float center_x = divider_left + divider_width * 0.5f;
  // The grab stays in the pane: past its edge it would take a press meant for
  // the next pane or the sash between them (soak 33efed186772).
  const float left = std::max(editor_surface.x, center_x - hit_width * 0.5f);
  const float right = std::min(editor_surface.x + editor_surface.w, center_x + hit_width * 0.5f);
  return MakeRect(left, editor_surface.y, std::max(0.0f, right - left), editor_surface.h);
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
