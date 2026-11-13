#include "workspace/SplitTreeLayout.h"

#include <algorithm>
#include <cmath>

namespace microide::workspace {

SplitRegionsLayout ComputeSplitRegions(const SplitTree& tree, const SDL_FRect& area, float divider) {
  SplitRegionsLayout result;
  if (!tree.is_split()) {
    result.panes.push_back(area);
    return result;
  }

  // Iterative depth-first walk, children left to right: the visit order of the
  // leaves is the leaf order, and the whole walk is heap-free (the stack is
  // bounded by the node budget, which the leaf cap bounds).
  struct Frame {
    std::uint8_t node = 0;
    SDL_FRect rect{};
  };
  util::InlineVector<Frame, SplitTree::kMaxNodes> stack;
  stack.push_back(Frame{tree.root(), area});
  while (!stack.empty()) {
    const Frame frame = stack.back();
    stack.pop_back();
    const SplitTree::Node& node = tree.node(frame.node);
    if (node.leaf()) {
      if (result.panes.size() < result.panes.capacity()) {
        result.panes.push_back(frame.rect);
      }
      continue;
    }

    const bool vertical = node.orientation == SplitOrientation::Vertical;
    const std::size_t count = node.children.size();
    const float span = vertical ? frame.rect.w : frame.rect.h;
    const float avail = std::max(0.0f, span - divider * static_cast<float>(count - 1));
    // Extents first: the divider between children i and i+1 needs both, and the
    // last child absorbs the rounding so the panes always tile the span exactly.
    util::InlineVector<float, kMaxSplitLeaves> extents;
    float remaining = avail;
    for (std::size_t i = 0; i < count; ++i) {
      const float extent = i + 1 == count
                               ? remaining
                               : std::clamp(std::floor(avail * node.weights[i]), 0.0f, remaining);
      extents.push_back(extent);
      remaining = std::max(0.0f, remaining - extent);
    }

    float cursor = vertical ? frame.rect.x : frame.rect.y;
    util::InlineVector<Frame, kMaxSplitLeaves> children;
    for (std::size_t i = 0; i < count; ++i) {
      const SDL_FRect child = vertical ? SDL_FRect{cursor, frame.rect.y, extents[i], frame.rect.h}
                                       : SDL_FRect{frame.rect.x, cursor, frame.rect.w, extents[i]};
      children.push_back(Frame{node.children[i], child});
      cursor += extents[i];
      if (i + 1 < count && result.dividers.size() < result.dividers.capacity()) {
        result.dividers.push_back(SplitDividerRect{
            .rect = vertical ? SDL_FRect{cursor, frame.rect.y, divider, frame.rect.h}
                             : SDL_FRect{frame.rect.x, cursor, frame.rect.w, divider},
            .node = frame.node,
            .boundary = static_cast<std::uint8_t>(i),
            .vertical = vertical,
            .pair_start = cursor - extents[i],
            .pair_extent = extents[i] + extents[i + 1],
        });
        cursor += divider;
      }
    }
    for (std::size_t i = children.size(); i > 0; --i) {
      stack.push_back(children[i - 1]);
    }
  }
  return result;
}

std::size_t SplitRegionIndexAt(std::span<const SDL_FRect> regions, float x, float y) {
  for (std::size_t i = 0; i < regions.size(); ++i) {
    const SDL_FRect& r = regions[i];
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
      return i;
    }
  }
  return kNoSplitRegion;
}

std::size_t AdjacentSplitRegion(std::span<const SDL_FRect> regions,
                                std::size_t from,
                                PaneDirection direction,
                                float slack) {
  if (from >= regions.size() || regions.size() < 2) {
    return kNoSplitRegion;
  }
  const SDL_FRect& self = regions[from];
  const bool horizontal = direction == PaneDirection::Left || direction == PaneDirection::Right;

  std::size_t best = kNoSplitRegion;
  float best_gap = 0.0f;
  float best_overlap = 0.0f;
  for (std::size_t i = 0; i < regions.size(); ++i) {
    if (i == from) {
      continue;
    }
    const SDL_FRect& other = regions[i];
    if (other.w <= 0.0f || other.h <= 0.0f) {
      continue;
    }
    float gap = 0.0f;
    switch (direction) {
      case PaneDirection::Left:
        gap = self.x - (other.x + other.w);
        break;
      case PaneDirection::Right:
        gap = other.x - (self.x + self.w);
        break;
      case PaneDirection::Up:
        gap = self.y - (other.y + other.h);
        break;
      case PaneDirection::Down:
        gap = other.y - (self.y + self.h);
        break;
    }
    if (gap < -slack) {
      continue;  // Overlaps or sits the other way; not on that side.
    }
    // How much of the CROSS axis the two share. A pane directly beside this one
    // beats one that merely starts further along the same row.
    const float overlap =
        horizontal ? std::max(0.0f, std::min(self.y + self.h, other.y + other.h) - std::max(self.y, other.y))
                   : std::max(0.0f, std::min(self.x + self.w, other.x + other.w) - std::max(self.x, other.x));
    if (overlap <= 0.0f) {
      continue;
    }
    if (best == kNoSplitRegion || gap < best_gap - slack ||
        (gap < best_gap + slack && overlap > best_overlap)) {
      best = i;
      best_gap = gap;
      best_overlap = overlap;
    }
  }
  return best;
}

}  // namespace microide::workspace
