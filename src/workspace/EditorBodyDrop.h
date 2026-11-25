#pragma once

#include <SDL3/SDL.h>

#include "workspace/state/WorkspaceInteractionState.h"

namespace microide::workspace {

struct EditorBodyDrop {
  EditorBodyDropZone zone = EditorBodyDropZone::Center;
  SDL_FRect region{};
};

EditorBodyDrop EditorBodyDropAt(const SDL_FRect& pane, float x, float y, bool can_split);

}
