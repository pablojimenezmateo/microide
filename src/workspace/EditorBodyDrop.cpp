#include "workspace/EditorBodyDrop.h"

#include <algorithm>

namespace microide::workspace {

namespace {

// How much of a pane's width/height at each edge reads as "split here" rather
// than "move into this pane". VS Code uses roughly a fifth; below that the centre
// target gets hard to hit on a narrow pane.
constexpr float kBodyDropEdgeFraction = 0.2f;

}  // namespace

EditorBodyDrop EditorBodyDropAt(const SDL_FRect& pane, float x, float y, bool can_split) {
  EditorBodyDrop drop{.zone = EditorBodyDropZone::Center, .region = pane};
  if (!can_split || pane.w <= 0.0f || pane.h <= 0.0f) {
    return drop;
  }
  const float left = (x - pane.x) / pane.w;
  const float top = (y - pane.y) / pane.h;
  const float right = 1.0f - left;
  const float bottom = 1.0f - top;
  const float nearest = std::min(std::min(left, right), std::min(top, bottom));
  if (nearest >= kBodyDropEdgeFraction) {
    return drop;
  }
  const float half_w = pane.w * 0.5f;
  const float half_h = pane.h * 0.5f;
  if (nearest == left) {
    return {EditorBodyDropZone::Left, SDL_FRect{pane.x, pane.y, half_w, pane.h}};
  }
  if (nearest == right) {
    return {EditorBodyDropZone::Right, SDL_FRect{pane.x + pane.w - half_w, pane.y, half_w, pane.h}};
  }
  if (nearest == top) {
    return {EditorBodyDropZone::Top, SDL_FRect{pane.x, pane.y, pane.w, half_h}};
  }
  return {EditorBodyDropZone::Bottom, SDL_FRect{pane.x, pane.y + pane.h - half_h, pane.w, half_h}};
}

}  // namespace microide::workspace
