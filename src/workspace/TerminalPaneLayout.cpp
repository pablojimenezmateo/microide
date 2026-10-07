#include "workspace/TerminalPaneLayout.h"

#include <algorithm>
#include <cmath>

namespace microide::workspace {

void NormalizeTerminalPaneWeights(TerminalPaneWeights& weights) {
  if (weights.empty()) {
    return;
  }
  float total = 0.0f;
  for (float weight : weights) {
    total += std::isfinite(weight) && weight > 0.0f ? weight : 0.0f;
  }
  if (total <= 0.0f) {
    const float even = 1.0f / static_cast<float>(weights.size());
    for (float& weight : weights) {
      weight = even;
    }
    return;
  }
  for (float& weight : weights) {
    weight = std::isfinite(weight) && weight > 0.0f ? weight / total : 0.0f;
  }
}

void InsertTerminalPaneWeight(TerminalPaneWeights& weights, std::size_t index) {
  if (weights.size() >= kMaxTerminalPanes) {
    return;
  }
  index = std::min(index, weights.size());
  if (weights.empty()) {
    weights.push_back(1.0f);
    return;
  }
  // The pane being split is the one the new pane sits next to: its left
  // neighbour, or the first pane when inserting at the front.
  const std::size_t source = index == 0 ? 0 : index - 1;
  const float half = weights[source] * 0.5f;
  weights[source] = half;
  weights.insert(index, half);
  NormalizeTerminalPaneWeights(weights);
}

void RemoveTerminalPaneWeight(TerminalPaneWeights& weights, std::size_t index) {
  if (index >= weights.size()) {
    return;
  }
  const float share = weights[index];
  weights.erase(index);
  if (weights.empty()) {
    return;
  }
  const std::size_t heir = index == 0 ? 0 : index - 1;
  weights[heir] += share;
  NormalizeTerminalPaneWeights(weights);
}

bool ResizeTerminalPaneDivider(TerminalPaneWeights& weights,
                               std::size_t boundary,
                               float first_share) {
  if (boundary + 1 >= weights.size() || !std::isfinite(first_share)) {
    return false;
  }
  const float pair = weights[boundary] + weights[boundary + 1];
  const float share = std::clamp(first_share, kTerminalPaneMinShare, 1.0f - kTerminalPaneMinShare);
  weights[boundary] = pair * share;
  weights[boundary + 1] = pair - weights[boundary];
  return true;
}

bool ResetTerminalPaneDivider(TerminalPaneWeights& weights, std::size_t boundary) {
  return ResizeTerminalPaneDivider(weights, boundary, 0.5f);
}

TerminalPaneRectsLayout ComputeTerminalPaneRects(const SDL_FRect& body,
                                                 std::span<const float> weights) {
  TerminalPaneRectsLayout layout;
  const std::size_t count = std::min(weights.size(), kMaxTerminalPanes);
  if (count == 0) {
    return layout;
  }
  if (count == 1) {
    layout.panes.push_back(body);
    return layout;
  }
  float total = 0.0f;
  for (std::size_t i = 0; i < count; ++i) {
    total += std::max(0.0f, weights[i]);
  }
  const bool even = total <= 0.0f;
  const float dividers_width = kTerminalPaneDividerThickness * static_cast<float>(count - 1);
  const float usable = std::max(0.0f, body.w - dividers_width);
  const float right_edge = body.x + body.w;
  float x = body.x;
  for (std::size_t i = 0; i < count; ++i) {
    const float share = even ? 1.0f / static_cast<float>(count) : std::max(0.0f, weights[i]) / total;
    const bool last = i + 1 == count;
    const float width = last ? std::max(0.0f, right_edge - x) : std::floor(usable * share);
    layout.panes.push_back(SDL_FRect{x, body.y, width, body.h});
    if (!last) {
      const float divider_x = x + width;
      layout.dividers.push_back(TerminalPaneDividerRect{
          .rect = SDL_FRect{divider_x, body.y, kTerminalPaneDividerThickness, body.h},
          .boundary = i,
          .pair_start = x,
          .pair_extent = 0.0f,  // filled below once the right pane's width is known
      });
      x = divider_x + kTerminalPaneDividerThickness;
    }
  }
  // A pair's span runs from the left pane's start to the right pane's end
  // (divider band included); the drag's share is expressed over that span.
  for (TerminalPaneDividerRect& divider : layout.dividers) {
    const SDL_FRect& right = layout.panes[divider.boundary + 1];
    divider.pair_extent = std::max(0.0f, right.x + right.w - divider.pair_start);
  }
  return layout;
}

std::size_t TerminalPaneIndexAt(const TerminalPaneRectsLayout& rects, float x, float y) {
  for (std::size_t i = 0; i < rects.panes.size(); ++i) {
    const SDL_FRect& pane = rects.panes[i];
    if (x >= pane.x && x < pane.x + pane.w && y >= pane.y && y < pane.y + pane.h) {
      return i;
    }
  }
  return rects.panes.size();
}

}  // namespace microide::workspace
