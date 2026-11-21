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

// The bottom panel's Output body -- the shown channel's rows, its empty hint and
// its scrollbar -- as a derived-damage surface (openspec derived-damage 3.1)
// painted element by element (clip-discipline stage 3). Free functions, as the
// header's (BottomPanelHeaderPaint.h).
namespace microide::workspace {

struct BottomPanelOutputInputs {
  SDL_Renderer* renderer = nullptr;  // for the device-pixel snap of unit rects
  bool visible = false;              // the panel shows Output and has a body
  const WorkspaceOutputChannels* channels = nullptr;
  std::string_view channel_id;
  const std::filesystem::path* project_root = nullptr;  // a snippet's path resolves against it
  const render::Theme* theme = nullptr;
  std::size_t text_metrics = 0;  // the text renderer's MetricsGeneration
  // The body's log layout, its scroll already settled for the frame.
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

// Settles what the Output body paints this frame into `frame.output_body` and
// derives its damage: each row, the hint and the scrollbar is a unit.
void PrepareBottomPanelOutputFrame(PreparedFrameState& frame, const BottomPanelOutputInputs& inputs);

// Draws `frame.output_body` and nothing else, each element inside its own
// render::PaintElement.
void PaintBottomPanelOutputBody(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                                const render::TextRenderer& text_renderer);

}  // namespace microide::workspace
