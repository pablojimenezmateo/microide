#pragma once

#include <cstdint>

#include <SDL3/SDL.h>

#include "workspace/SplitTree.h"

namespace microide::workspace {

// Where something dragged over a pane's body would land, VS Code style. `Center`
// moves it into that pane (an editor group takes the tab); the four edge zones
// carve a new pane out of that one on that side. `None` means the pointer is not
// offering a body drop at all -- it is over a strip, or over a pane where the
// drop would be a no-op.
enum class PaneDropZone : std::uint8_t {
  None,
  Center,
  Left,
  Right,
  Top,
  Bottom,
};

// The split an edge zone asks for: its axis and whether the new pane goes on the
// target's leading side. Meaningless for `Center` / `None` (`edge` false).
struct PaneDropSplit {
  bool edge = false;
  SplitOrientation orientation = SplitOrientation::None;
  bool before = false;
};
constexpr PaneDropSplit SplitForPaneDropZone(PaneDropZone zone) {
  switch (zone) {
    case PaneDropZone::Left:
      return {true, SplitOrientation::Vertical, true};
    case PaneDropZone::Right:
      return {true, SplitOrientation::Vertical, false};
    case PaneDropZone::Top:
      return {true, SplitOrientation::Horizontal, true};
    case PaneDropZone::Bottom:
      return {true, SplitOrientation::Horizontal, false};
    case PaneDropZone::None:
    case PaneDropZone::Center:
      break;
  }
  return {};
}

// Where something dragged over a pane's body would land: within a fifth of the
// pane's width or height of an edge, that side (the new pane takes half of this
// one); anywhere else, the pane itself. A dragged editor tab, a file dragged from
// the tree and a dragged terminal tab resolve their drop with this one rule.
struct PaneDrop {
  PaneDropZone zone = PaneDropZone::Center;
  // The region the drop preview paints: the half an edge drop carves, or the pane.
  SDL_FRect region{};
};

// `can_split` false offers only the centre (the surface is full, or the drop
// would carve a pane out of itself). `can_center` false offers no centre: the
// middle of the pane answers `None` (a terminal pane holds one session, so
// "into this pane" has no meaning there).
PaneDrop PaneDropAt(const SDL_FRect& pane, float x, float y, bool can_split, bool can_center = true);

}  // namespace microide::workspace
