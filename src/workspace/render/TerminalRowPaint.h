#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "terminal/TerminalCell.h"
#include "terminal/TerminalSearch.h"
#include "workspace/WorkspaceTerminalSelection.h"

// What one terminal row paints: background fills, glyph runs and decoration
// strokes, each over a span of grid columns and already in its final colour.
//
// The paint loop used to resolve a cell's colours inside three run-length passes
// (fills, glyphs, decorations), asking the shell per cell whether the cell was
// selected -- a project-state walk per call, about five calls per cell per frame
// -- and an emphasized cell threw its own colours away: a selection painted the
// theme's hover tint under the theme's text colour, so red output turned grey and
// a coloured background vanished inside it. Here every cell is resolved once, the
// emphasis is a wash composited over the cell's own background, and the cell keeps
// its foreground (moved only as far as legibility needs). Pure: no renderer, no
// state, so the rules are unit-tested and the render TU only draws the plan.
namespace microide::workspace {

// The colours a row resolves against.
struct TerminalPaintPalette {
  SDL_Color foreground{};  // an unstyled cell's text
  SDL_Color background{};  // an unstyled cell's background
  // Translucent washes composited over a cell's own background.
  SDL_Color selection{};
  SDL_Color find_match{};
  SDL_Color find_current{};
  // DECSCNM: the whole screen in reverse video.
  bool reverse_video = false;

  // What the pane body under the rows is already filled with, so a fill of this
  // colour is not drawn at all.
  SDL_Color base() const { return reverse_video ? foreground : background; }
};

// The selection wash for a terminal that does not own the keyboard: half as
// strong, so a selection left in an unfocused terminal reads as kept but not live
// (VS Code's inactive selection).
SDL_Color InactiveTerminalSelection(SDL_Color selection);

struct TerminalCellColors {
  SDL_Color foreground{};
  SDL_Color background{};
};

// A cell's colours with no emphasis: the theme's defaults, SGR inverse, dim and
// hidden, and DECSCNM.
TerminalCellColors ResolveTerminalCellColors(const terminal::TerminalStyle& style,
                                             const TerminalPaintPalette& palette);

// The emphasis one row carries.
struct TerminalRowEmphasis {
  static constexpr std::size_t kToRowEnd = std::numeric_limits<std::size_t>::max();
  static constexpr std::size_t kNoMatch = std::numeric_limits<std::size_t>::max();

  // Selected columns, half-open; empty when begin == end.
  std::size_t selection_begin = 0;
  std::size_t selection_end = 0;
  // The selection continues on the next row, so the row's line break is selected
  // too: the wash runs on past the row's last cell to the pane's edge, as in every
  // terminal, and the copy ends this row with a newline.
  bool selection_runs_past_row = false;
  // This row's find matches, (column)-ordered, and which of them is current.
  const terminal::TerminalSearchMatch* matches = nullptr;
  std::size_t match_count = 0;
  std::size_t current_match = kNoMatch;
};

// The part of `selection` on absolute row `row`. Matches what
// ExtractTerminalSelectionText copies, so what is painted is what is copied.
TerminalRowEmphasis TerminalRowSelectionEmphasis(const std::optional<TerminalSelectionBounds>& selection,
                                                 std::size_t row);

// Minimum contrast an emphasized cell's text keeps against its washed background
// (WCAG AA; VS Code's terminal.integrated.minimumContrastRatio default).
inline constexpr float kTerminalEmphasisMinimumContrast = 4.5f;

struct TerminalRowPaintPlan {
  struct Fill {
    std::size_t begin = 0;
    std::size_t end = 0;
    SDL_Color color{};
  };
  struct TextRun {
    std::size_t column = 0;
    SDL_Color color{};
    std::size_t offset = 0;
    std::size_t length = 0;
  };
  struct Decoration {
    std::size_t begin = 0;
    std::size_t end = 0;
    SDL_Color color{};
    std::uint16_t bits = 0;
  };

  std::vector<Fill> fills;
  std::vector<TextRun> text_runs;
  std::vector<Decoration> decorations;

  std::string_view RunText(const TextRun& run) const {
    return std::string_view(text).substr(run.offset, run.length);
  }

  // The glyph runs' bytes. Everything here is reused across rows and frames, so
  // planning a row allocates nothing once warm.
  std::string text;
  // Working state of PlanTerminalRow, not part of the plan.
  struct Scratch {
    std::vector<TerminalCellColors> cells;
    // One-entry memo of the contrast correction: a selected span is mostly one
    // (foreground, background) pair, and the correction costs a dozen pow() calls.
    SDL_Color memo_foreground{};
    SDL_Color memo_background{};
    SDL_Color memo_result{};
    bool memo_valid = false;
  } scratch;
};

// Plans `line` across a pane `columns` cells wide. Columns past the row's cells
// paint only the selection wash of a row the selection runs past.
void PlanTerminalRow(const terminal::TerminalLine& line,
                     std::size_t columns,
                     const TerminalRowEmphasis& emphasis,
                     const TerminalPaintPalette& palette,
                     TerminalRowPaintPlan& plan);

}  // namespace microide::workspace
