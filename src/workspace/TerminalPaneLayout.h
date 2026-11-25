#pragma once

#include <cstddef>
#include <span>

#include <SDL3/SDL.h>

#include "util/InlineVector.h"

namespace microide::workspace {

inline constexpr std::size_t kMaxTerminalPanes = 6;

inline constexpr float kTerminalPaneDividerThickness = 6.0f;

inline constexpr float kTerminalPaneMinShare = 0.1f;

using TerminalPaneWeights = util::InlineVector<float, kMaxTerminalPanes>;

void NormalizeTerminalPaneWeights(TerminalPaneWeights& weights);

void InsertTerminalPaneWeight(TerminalPaneWeights& weights, std::size_t index);

void RemoveTerminalPaneWeight(TerminalPaneWeights& weights, std::size_t index);

bool ResizeTerminalPaneDivider(TerminalPaneWeights& weights,
                               std::size_t boundary,
                               float first_share);

bool ResetTerminalPaneDivider(TerminalPaneWeights& weights, std::size_t boundary);

struct TerminalPaneDividerRect {
  SDL_FRect rect{};
  std::size_t boundary = 0;
  float pair_start = 0.0f;
  float pair_extent = 0.0f;
};

struct TerminalPaneRectsLayout {
  util::InlineVector<SDL_FRect, kMaxTerminalPanes> panes;
  util::InlineVector<TerminalPaneDividerRect, kMaxTerminalPanes - 1> dividers;
};

TerminalPaneRectsLayout ComputeTerminalPaneRects(const SDL_FRect& body,
                                                 std::span<const float> weights);

std::size_t TerminalPaneIndexAt(const TerminalPaneRectsLayout& rects, float x, float y);

}
