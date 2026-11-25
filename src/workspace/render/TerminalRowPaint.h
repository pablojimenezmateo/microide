#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "render/TextRendererBackend.h"
#include "terminal/TerminalCell.h"
#include "terminal/TerminalSearch.h"
#include "workspace/WorkspaceTerminalSelection.h"

namespace microide::workspace {

struct TerminalPaintPalette {
  SDL_Color foreground{};
  SDL_Color background{};
  SDL_Color selection{};
  SDL_Color find_match{};
  SDL_Color find_current{};
  bool reverse_video = false;

  SDL_Color base() const { return reverse_video ? foreground : background; }
};

SDL_Color InactiveTerminalSelection(SDL_Color selection);

struct TerminalCellColors {
  SDL_Color foreground{};
  SDL_Color background{};
};

TerminalCellColors ResolveTerminalCellColors(const terminal::TerminalStyle& style,
                                             const TerminalPaintPalette& palette);

struct TerminalRowEmphasis {
  static constexpr std::size_t kToRowEnd = std::numeric_limits<std::size_t>::max();
  static constexpr std::size_t kNoMatch = std::numeric_limits<std::size_t>::max();

  std::size_t selection_begin = 0;
  std::size_t selection_end = 0;
  bool selection_runs_past_row = false;
  const terminal::TerminalSearchMatch* matches = nullptr;
  std::size_t match_count = 0;
  std::size_t current_match = kNoMatch;
};

TerminalRowEmphasis TerminalRowSelectionEmphasis(const std::optional<TerminalSelectionBounds>& selection,
                                                 std::size_t row);

SDL_FRect TerminalCursorShapeRect(SDL_FRect cell, terminal::TerminalCursorShape shape);

inline constexpr float kTerminalEmphasisMinimumContrast = 4.5f;

struct TerminalRowPaintPlan {
  struct Fill {
    std::size_t begin = 0;
    std::size_t end = 0;
    SDL_Color color{};
  };
  struct TextRun {
    std::size_t column = 0;
    std::size_t end = 0;
    SDL_Color color{};
    render::FontStyle face = render::FontStyle::Regular;
    std::size_t offset = 0;
    std::size_t length = 0;
  };
  struct Decoration {
    std::size_t begin = 0;
    std::size_t end = 0;
    SDL_Color color{};
    SDL_Color underline_color{};
    std::uint16_t bits = 0;
    terminal::TerminalUnderlineVariant variant = terminal::TerminalUnderlineVariant::Straight;
  };

  std::vector<Fill> fills;
  std::vector<TextRun> text_runs;
  std::vector<Decoration> decorations;

  std::string_view RunText(const TextRun& run) const {
    return std::string_view(text).substr(run.offset, run.length);
  }

  std::string text;
  struct Scratch {
    std::vector<TerminalCellColors> cells;
    struct ContrastEntry {
      std::uint64_t key = 0;
      SDL_Color result{};
    };
    std::array<ContrastEntry, 64> contrast{};
  } scratch;
};

void PlanTerminalRow(const terminal::TerminalLine& line,
                     std::size_t columns,
                     const TerminalRowEmphasis& emphasis,
                     const TerminalPaintPalette& palette,
                     TerminalRowPaintPlan& plan,
                     std::span<const terminal::TerminalUnderlineColor> underline_colors = {});

}
