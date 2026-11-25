#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <string_view>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/render/PreparedFrameState.h"
#include "workspace/render/RenderViewModelBuilder.h"

namespace microide::workspace {

struct BottomPanelHeaderInputs {
  SDL_Renderer* renderer = nullptr;
  const WorkspaceLayout* layout = nullptr;
  const BottomPanelSurfaceViewModel* panel = nullptr;
  const render::Theme* theme = nullptr;
  std::string_view label;
  SDL_FRect new_terminal_rect{};
  SDL_FRect maximize_rect{};
  bool divider_dragged = false;
  std::function<bool(const SDL_FRect&)> pointer_over;
  std::function<void(const SDL_FRect&)> damage;
};

void PrepareBottomPanelHeaderFrame(PreparedFrameState& frame, const BottomPanelHeaderInputs& inputs);

void PaintBottomPanelHeader(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                            const render::TextRenderer& text_renderer);

}
