#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>
#include <span>

#include "workspace/WorkspaceLayout.h"
#include "workspace/registries/WorkspaceMenuRegistry.h"

namespace microide::workspace {

inline constexpr float kPopupInset = 6.0f;
inline constexpr float kPopupColumnGap = 8.0f;

inline float PopupRowHeight(const MenuItemSpec& item) {
  return item.separator ? kWorkspaceMenuPopupSeparatorHeight : kWorkspaceMenuPopupItemHeight;
}

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

struct PopupRowGeometry {
  std::size_t index = 0;
  SDL_FRect rect{};
  bool separator = false;
};
std::optional<PopupRowGeometry> HitTestPopupRow(std::span<const MenuItemSpec> items, const SDL_FRect& popup_rect,
                                                float x, float y);
std::optional<SDL_FRect> PopupRowRectByIndex(std::span<const MenuItemSpec> items, const SDL_FRect& popup_rect,
                                             std::size_t index);

}
