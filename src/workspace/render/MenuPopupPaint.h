#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <functional>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/render/DerivedDamage.h"
#include "workspace/render/PreparedFrameState.h"

namespace microide::workspace {

constexpr DerivedSurface MenuPopupSurface(MenuPopupSlot slot) {
  switch (slot) {
    case MenuPopupSlot::Popup:
      return DerivedSurface::MenuPopup;
    case MenuPopupSlot::Submenu:
      return DerivedSurface::MenuSubmenu;
    case MenuPopupSlot::Overflow:
      return DerivedSurface::MenuOverflowPopup;
    case MenuPopupSlot::Context:
    case MenuPopupSlot::Count:
      break;
  }
  return DerivedSurface::ContextMenu;
}

void CommitMenuPopupUnits(PreparedFrameState& frame, SDL_Renderer* renderer, const render::Theme& theme,
                          std::uint64_t text_metrics, const std::function<void(const SDL_FRect&)>& damage);

void PaintMenuPopups(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                     const render::TextRenderer& text_renderer);

}
