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

// The terminal's chrome around its grid -- each pane's empty hint and scrollbar,
// the dividers between panes, the hint over a panel with no session -- as a
// derived-damage surface (openspec derived-damage 3.1) painted element by element
// (clip-discipline stage 3). The grid is its own surface (BottomPanelTerminalGridPaint.h).
// Free functions, as the header's (BottomPanelHeaderPaint.h).
namespace microide::workspace {

struct TerminalPaneChromeInputs {
  SDL_FRect text_box{};     // where a hint wraps: the pane's text column, to its bottom
  bool shows_hint = false;  // the active pane, with no lines yet
  std::optional<ScrollbarGeometry> scrollbar;
  bool scrollbar_active = false;
};

struct TerminalChromeInputs {
  SDL_Renderer* renderer = nullptr;  // for the device-pixel snap of unit rects
  bool visible = false;              // the panel shows Terminal and has a body
  const render::Theme* theme = nullptr;
  std::size_t text_metrics = 0;  // the text renderer's MetricsGeneration
  std::string_view empty_label;
  std::optional<SDL_FRect> no_session_box;  // set when there is no live session
  util::InlineVector<TerminalPaneChromeInputs, kMaxTerminalPanes> panes;
  util::InlineVector<PreparedFrameState::TerminalDividerPaint, kMaxTerminalPanes> dividers;
  std::function<void(const SDL_FRect&)> damage;
};

// Settles the chrome into `frame.terminal_chrome` and derives its damage: the
// no-session hint, each pane's hint and scrollbar, and each divider is a unit.
void PrepareTerminalChromeFrame(PreparedFrameState& frame, const TerminalChromeInputs& inputs);

// The hint over a panel with no session, and a pane's hint: drawn where the
// pane's rows are, before its caret.
void PaintTerminalNoSessionHint(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                                const render::TextRenderer& text_renderer);
void PaintTerminalPaneHint(SDL_Renderer* renderer, const PreparedFrameState& frame, std::size_t pane,
                           const render::Theme& theme, const render::TextRenderer& text_renderer);

// The scrollbars and dividers, over every pane.
void PaintTerminalChrome(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme);

}  // namespace microide::workspace
