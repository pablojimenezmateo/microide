#pragma once

#include <SDL3/SDL.h>

#include <functional>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/render/PreparedFrameState.h"
#include "workspace/render/RenderViewModelBuilder.h"

namespace microide::workspace {

struct SettingsCardInputs {
  SDL_Renderer* renderer = nullptr;
  const render::Theme* theme = nullptr;
  const render::TextRenderer* text_renderer = nullptr;
  SettingsOverlayViewModel vm;
  SDL_FRect backdrop{};
  std::function<bool(const SDL_FRect&)> pointer_over;
  bool scrollbar_dragged = false;
  bool category_scrollbar_dragged = false;
  bool picker_scrollbar_dragged = false;
  std::function<void(const SDL_FRect&)> damage;
};

void PrepareSettingsCardFrame(PreparedFrameState& frame, SettingsCardInputs inputs);

}
