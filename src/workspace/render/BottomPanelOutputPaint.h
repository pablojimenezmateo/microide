#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/render/PreparedFrameState.h"

namespace microide::workspace {

struct BottomPanelOutputInputs {
  SDL_Renderer* renderer = nullptr;
  bool visible = false;
  const WorkspaceOutputChannels* channels = nullptr;
  std::string_view channel_id;
  const std::filesystem::path* project_root = nullptr;
  const render::Theme* theme = nullptr;
  std::size_t text_metrics = 0;
  SDL_FRect content{};
  float text_x = 0.0f;
  float text_y = 0.0f;
  float text_width = 0.0f;
  float line_height = 0.0f;
  int first_row = 0;
  int visible_rows = 0;
  std::optional<ScrollbarGeometry> scrollbar;
  bool scrollbar_active = false;
  std::string_view empty_label;
  std::function<void(const SDL_FRect&)> damage;
};

void PrepareBottomPanelOutputFrame(PreparedFrameState& frame, const BottomPanelOutputInputs& inputs);

void PaintBottomPanelOutputBody(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                                const render::TextRenderer& text_renderer);

}
