#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "terminal/TerminalCell.h"
#include "terminal/TerminalSearch.h"
#include "util/InlineVector.h"
#include "workspace/TerminalPaneLayout.h"
#include "workspace/WorkspaceTerminalSelection.h"
#include "workspace/render/PreparedFrameState.h"
#include "workspace/render/TerminalRowPaint.h"

namespace microide::workspace {

struct TerminalGridPaneInputs {
  SDL_FRect body{};
  float text_x = 0.0f;
  float text_y = 0.0f;
  float text_width = 0.0f;
  float line_height = 0.0f;
  std::size_t first_row = 0;
  std::size_t visible_rows = 0;
  std::size_t line_count = 0;
  std::span<const terminal::TerminalLine> lines;
  std::span<const std::uint64_t> row_keys;
  std::span<const terminal::TerminalUnderlineColor> underline_colors;
  bool reverse_video = false;
  std::optional<TerminalSelectionBounds> selection;
  bool selection_live = false;
  std::optional<TerminalLinkSpan> hovered_link;
  std::span<const terminal::TerminalSearchMatch> find_matches;
  std::size_t find_selected = TerminalRowEmphasis::kNoMatch;
  std::optional<SDL_FRect> caret_cell;
  terminal::TerminalCursorSnapshot cursor{};
  bool caret_owned = false;
};

struct TerminalGridInputs {
  SDL_Renderer* renderer = nullptr;
  bool visible = false;
  const render::Theme* theme = nullptr;
  std::size_t text_metrics = 0;
  float char_width = 0.0f;
  const render::TextRenderer* terminal_text = nullptr;
  bool caret_phase_on = true;
  util::InlineVector<TerminalGridPaneInputs, kMaxTerminalPanes> panes;
  std::function<void(const SDL_FRect&)> damage;
};

void KeyTerminalGridRows(std::span<const terminal::TerminalLine> lines, std::vector<std::uint64_t>& keys);

void PrepareTerminalGridFrame(PreparedFrameState& frame, const TerminalGridInputs& inputs);

void PaintTerminalGridPane(SDL_Renderer* renderer, const PreparedFrameState& frame, std::size_t pane,
                           const render::Theme& theme, const render::TextRenderer& terminal_text,
                           TerminalRowPaintPlan& plan);

void PaintTerminalGridCaret(SDL_Renderer* renderer, const PreparedFrameState& frame, std::size_t pane,
                            const render::Theme& theme, const render::TextRenderer& terminal_text);

}
