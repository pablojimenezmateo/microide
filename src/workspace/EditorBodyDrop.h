#pragma once

#include <SDL3/SDL.h>

#include "workspace/state/WorkspaceInteractionState.h"

namespace microide::workspace {

// Where something dragged over an editor pane's body would land, VS Code style:
// within a fifth of the pane's width or height of an edge, that side (the new
// pane takes half of this one); anywhere else, the pane itself. A dragged tab
// and a file dragged from the tree resolve their drop with this one rule.
struct EditorBodyDrop {
  EditorBodyDropZone zone = EditorBodyDropZone::Center;
  // The region the drop preview paints: the half an edge drop carves, or the pane.
  SDL_FRect region{};
};

// `can_split` false offers only the centre (the editor area is full, or the drop
// would carve a pane out of itself).
EditorBodyDrop EditorBodyDropAt(const SDL_FRect& pane, float x, float y, bool can_split);

}  // namespace microide::workspace
