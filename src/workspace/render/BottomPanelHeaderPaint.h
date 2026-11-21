#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <string_view>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/render/PreparedFrameState.h"
#include "workspace/render/RenderViewModelBuilder.h"

// The bottom panel's header -- its tabs, overflow chevrons, label, and the
// new-terminal and maximize buttons -- as a derived-damage surface (openspec
// derived-damage 3.1) painted element by element (clip-discipline stage 3). Free
// functions: the shell's member list is at its cap, and a paint that cannot reach
// the shell is one that reads only its snapshot.
namespace microide::workspace {

struct BottomPanelHeaderInputs {
  SDL_Renderer* renderer = nullptr;  // for the device-pixel snap of unit rects
  const WorkspaceLayout* layout = nullptr;
  const BottomPanelSurfaceViewModel* panel = nullptr;
  const render::Theme* theme = nullptr;
  std::string_view label;  // the header's text when it has no tabs
  SDL_FRect new_terminal_rect{};
  SDL_FRect maximize_rect{};
  std::function<bool(const SDL_FRect&)> pointer_over;
  std::function<void(const SDL_FRect&)> damage;
};

// Settles what the header paints this frame into `frame.panel_header` and derives
// its damage: each tab and button is a unit whose rect is the element's snapped
// rect and whose fingerprint is everything it paints.
void PrepareBottomPanelHeaderFrame(PreparedFrameState& frame, const BottomPanelHeaderInputs& inputs);

// Draws `frame.panel_header` and nothing else, each element inside its own
// render::PaintElement.
void PaintBottomPanelHeader(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                            const render::TextRenderer& text_renderer);

}  // namespace microide::workspace
