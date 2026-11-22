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

// The right-side debug pane's body -- mode tabs, the active surface's rows (Call
// Stack, Variables, Watch, Breakpoints), its empty hint and scrollbar -- as a
// derived-damage surface (openspec derived-damage 3.1) painted element by element
// (clip-discipline stage 3). Free functions, as the bottom panel's
// (BottomPanelHeaderPaint.h).
namespace microide::workspace {

struct DebugPaneInputs {
  SDL_Renderer* renderer = nullptr;  // for the device-pixel snap of unit rects
  bool visible = false;              // the debugger is on and the pane has room
  const DebugPaneSurfaceViewModel* pane = nullptr;
  const render::Theme* theme = nullptr;
  const render::TextRenderer* text_renderer = nullptr;  // rows are laid out (measured) here
  DebugPaneModeRowLayout mode_row;
  // The list's log layout, its scroll already settled for the frame.
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
  bool edit_caret_shown = false;  // the window has the keyboard and no composition is open
  std::function<bool(const SDL_FRect&)> pointer_over;
  std::function<void(const SDL_FRect&)> damage;
};

// Lays the pane out into `frame.debug_pane` and derives its damage: each mode tab,
// each visible row, the hint and the scrollbar is a unit.
void PrepareDebugPaneFrame(PreparedFrameState& frame, const DebugPaneInputs& inputs);

// Draws `frame.debug_pane` and nothing else, each element inside its own
// render::PaintElement.
void PaintDebugPaneBody(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                        const render::TextRenderer& text_renderer);

}  // namespace microide::workspace
