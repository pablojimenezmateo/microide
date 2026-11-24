#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>
#include <span>

#include "workspace/WorkspaceLayout.h"
#include "workspace/registries/WorkspaceMenuRegistry.h"

namespace microide::workspace {

// Popup rows are laid out from the popup's rect alone, so render, hit-test,
// keyboard reveal and the `ui` query agree by construction. A popup taller than
// the window could otherwise give its rows (the Edit menu's 31 at 700 px, the
// Debug menu's 21 at 480 px) rects below the window edge, where nothing can
// reach them: ComputePopupMenuRect then caps the height at what fits, and the
// rows break into further columns, as classic desktop menus do.
inline constexpr float kPopupInset = 6.0f;
inline constexpr float kPopupColumnGap = 8.0f;

inline float PopupRowHeight(const MenuItemSpec& item) {
  return item.separator ? kWorkspaceMenuPopupSeparatorHeight : kWorkspaceMenuPopupItemHeight;
}

// Calls fn(index, rect) for every row in order, flowing into a new column when a
// row would pass the popup's bottom inset. The column count is found by the same
// walk, so the columns split the width evenly.
template <typename Fn>
void ForEachPopupRow(std::span<const MenuItemSpec> items, const SDL_FRect& popup, Fn&& fn) {
  const float top = popup.y + kPopupInset;
  const float bottom = popup.y + popup.h - kPopupInset;
  std::size_t columns = 1;
  {
    float y = top;
    for (const MenuItemSpec& item : items) {
      const float height = PopupRowHeight(item);
      if (y + height > bottom + 0.5f && y > top) {
        ++columns;
        y = top;
      }
      y += height;
    }
  }
  const float column_w = std::max(
      0.0f, (popup.w - kPopupInset * 2.0f - kPopupColumnGap * static_cast<float>(columns - 1)) /
                static_cast<float>(columns));
  float x = popup.x + kPopupInset;
  float y = top;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const float height = PopupRowHeight(items[i]);
    if (y + height > bottom + 0.5f && y > top) {
      x += column_w + kPopupColumnGap;
      y = top;
    }
    if (!fn(i, SDL_FRect{x, y, column_w, height})) {
      return;
    }
    y += height;
  }
}

// How many columns `items` need at a popup height of `height`.
inline std::size_t PopupColumnsAtHeight(std::span<const MenuItemSpec> items, float height) {
  std::size_t columns = 1;
  float y = 0.0f;
  const float bottom = height - kPopupInset * 2.0f;
  for (const MenuItemSpec& item : items) {
    const float row = PopupRowHeight(item);
    if (y + row > bottom + 0.5f && y > 0.0f) {
      ++columns;
      y = 0.0f;
    }
    y += row;
  }
  return columns;
}

// A popup row under a point, or by index, from the popup's rect alone (no text
// measuring): the per-event hit tests the menus run on every pointer motion.
struct PopupRowGeometry {
  std::size_t index = 0;
  SDL_FRect rect{};
  bool separator = false;
};
std::optional<PopupRowGeometry> HitTestPopupRow(std::span<const MenuItemSpec> items, const SDL_FRect& popup_rect,
                                                float x, float y);
std::optional<SDL_FRect> PopupRowRectByIndex(std::span<const MenuItemSpec> items, const SDL_FRect& popup_rect,
                                             std::size_t index);

}  // namespace microide::workspace
