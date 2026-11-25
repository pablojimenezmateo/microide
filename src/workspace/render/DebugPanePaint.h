#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <functional>
#include <optional>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/debug/DebugPaneRegistry.h"
#include "workspace/render/PreparedFrameState.h"
#include "workspace/render/RenderViewModelBuilder.h"

namespace microide::workspace {

struct DebugPaneInputs {
  SDL_Renderer* renderer = nullptr;
  bool visible = false;
  const DebugPaneSurfaceViewModel* pane = nullptr;
  const render::Theme* theme = nullptr;
  const render::TextRenderer* text_renderer = nullptr;
  DebugPaneModeRowLayout mode_row;
  SDL_FRect content{};
  float text_x = 0.0f;
  float text_y = 0.0f;
  float text_width = 0.0f;
  float line_height = 0.0f;
  int first_row = 0;
  int visible_rows = 0;
  std::size_t line_count = 0;
  std::optional<ScrollbarGeometry> scrollbar;
  bool scrollbar_active = false;
  bool edit_caret_shown = false;
  std::function<bool(const SDL_FRect&)> pointer_over;
  std::function<void(const SDL_FRect&)> damage;
};

void PrepareDebugPaneFrame(PreparedFrameState& frame, const DebugPaneInputs& inputs);

void PaintDebugPaneBody(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                        const render::TextRenderer& text_renderer);

}
