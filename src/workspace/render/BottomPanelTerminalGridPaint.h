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

// The terminal's grid as a derived-damage surface: every visible row of every
// pane is a unit, keyed on its cells and on what the frame paints over them --
// the selection's slice, its find matches, the hovered link's underline -- so a
// frame repaints the rows that changed, whoever changed them. The grid used to
// repaint a whole pane for any new output (a keystroke's echo is one row) and
// for any selection change, and a hovered link had no damage of its own.
// Free functions, as the terminal's chrome (BottomPanelTerminalChromePaint.h).
namespace microide::workspace {

struct TerminalGridPaneInputs {
  SDL_FRect body{};  // the pane's slice of the panel body
  float text_x = 0.0f;
  float text_y = 0.0f;  // the first row's text
  float text_width = 0.0f;
  float line_height = 0.0f;
  std::size_t first_row = 0;     // the absolute row of `lines[0]`
  std::size_t visible_rows = 0;  // the pane's row slots; a slot past the lines has no unit rect
  std::size_t line_count = 0;    // the session's lines as the frame sampled them
  // The pane's frame snapshot and each line's content key (KeyTerminalGridRows).
  std::span<const terminal::TerminalLine> lines;
  std::span<const std::uint64_t> row_keys;
  std::span<const terminal::TerminalUnderlineColor> underline_colors;
  bool reverse_video = false;  // DECSCNM
  std::optional<TerminalSelectionBounds> selection;
  bool selection_live = false;  // the panel has the keyboard: the wash at full strength
  std::optional<TerminalLinkSpan> hovered_link;
  // The find matches the pane highlights, (row, column)-ordered, and the current
  // one's index in them (TerminalRowEmphasis::kNoMatch: none).
  std::span<const terminal::TerminalSearchMatch> find_matches;
  std::size_t find_selected = TerminalRowEmphasis::kNoMatch;
};

struct TerminalGridInputs {
  SDL_Renderer* renderer = nullptr;  // for the device-pixel snap of unit rects
  bool visible = false;              // the panel shows a terminal tab with panes
  const render::Theme* theme = nullptr;
  std::size_t text_metrics = 0;  // the terminal text renderer's MetricsGeneration
  float char_width = 0.0f;       // its cell advance
  util::InlineVector<TerminalGridPaneInputs, kMaxTerminalPanes> panes;  // one per pane of the tab
  std::function<void(const SDL_FRect&)> damage;
};

// A content key per line: its cells' glyphs and styles. Run when a pane takes a
// new snapshot, not per frame; `keys` keeps its capacity.
void KeyTerminalGridRows(std::span<const terminal::TerminalLine> lines, std::vector<std::uint64_t>& keys);

// Settles the grid into `frame.terminal_grid` and derives its damage: each pane
// is a unit for its body (the blank a reverse-video screen fills) and then one
// per row slot.
void PrepareTerminalGridFrame(PreparedFrameState& frame, const TerminalGridInputs& inputs);

// One pane's body fill and rows. `plan` is the caller's row scratch.
void PaintTerminalGridPane(SDL_Renderer* renderer, const PreparedFrameState& frame, std::size_t pane,
                           const render::Theme& theme, const render::TextRenderer& terminal_text,
                           TerminalRowPaintPlan& plan);

// The palette pane `pane` resolves its cells against this frame: what its rows
// were painted with, for the caret drawn over them.
TerminalPaintPalette TerminalGridPanePalette(const PreparedFrameState& frame, std::size_t pane,
                                             const render::Theme& theme);

}  // namespace microide::workspace
