#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "util/InlineVector.h"
#include "workspace/TerminalPaneLayout.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/render/PreparedFrameState.h"

namespace microide::workspace {

struct TerminalPaneChromeInputs {
  SDL_FRect text_box{};
  bool shows_hint = false;
  std::optional<ScrollbarGeometry> scrollbar;
  bool scrollbar_active = false;
};

struct TerminalChromeInputs {
  SDL_Renderer* renderer = nullptr;
  bool visible = false;
  const render::Theme* theme = nullptr;
  std::size_t text_metrics = 0;
  std::string_view empty_label;
  std::optional<SDL_FRect> no_session_box;
  util::InlineVector<TerminalPaneChromeInputs, kMaxTerminalPanes> panes;
  util::InlineVector<PreparedFrameState::TerminalDividerPaint, kMaxTerminalPanes> dividers;
  std::function<void(const SDL_FRect&)> damage;
};

void PrepareTerminalChromeFrame(PreparedFrameState& frame, const TerminalChromeInputs& inputs);

void PaintTerminalNoSessionHint(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                                const render::TextRenderer& text_renderer);
void PaintTerminalPaneHint(SDL_Renderer* renderer, const PreparedFrameState& frame, std::size_t pane,
                           const render::Theme& theme, const render::TextRenderer& text_renderer);

void PaintTerminalChrome(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme);

}
