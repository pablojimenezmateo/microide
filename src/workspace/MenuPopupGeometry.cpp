#include "workspace/MenuPopupGeometry.h"

namespace microide::workspace {

std::optional<PopupRowGeometry> HitTestPopupRow(std::span<const MenuItemSpec> items, const SDL_FRect& popup_rect,
                                                float x, float y) {
  if (x < popup_rect.x || x >= popup_rect.x + popup_rect.w || y < popup_rect.y || y >= popup_rect.y + popup_rect.h) {
    return std::nullopt;
  }
  std::optional<PopupRowGeometry> hit;
  ForEachPopupRow(items, popup_rect, [&](std::size_t i, const SDL_FRect& rect) {
    if (Contains(rect, x, y)) {
      hit = PopupRowGeometry{.index = i, .rect = rect, .separator = items[i].separator};
      return false;
    }
    return true;
  });
  return hit;
}

std::optional<SDL_FRect> PopupRowRectByIndex(std::span<const MenuItemSpec> items, const SDL_FRect& popup_rect,
                                             std::size_t index) {
  if (index >= items.size()) {
    return std::nullopt;
  }
  std::optional<SDL_FRect> found;
  ForEachPopupRow(items, popup_rect, [&](std::size_t i, const SDL_FRect& rect) {
    if (i == index) {
      found = rect;
      return false;
    }
    return true;
  });
  return found;
}

}
