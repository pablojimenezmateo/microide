#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <functional>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/render/DerivedDamage.h"
#include "workspace/render/PreparedFrameState.h"

// The open menus -- the menu bar's popup, its submenu, the compact layout's
// overflow popup and the context menu (the tree's, a tab's and the terminal's
// share it) -- as derived-damage surfaces painted row by row. Frame prep lays
// each open one out into `frame.menu_popups` (the shell resolves the rows: their
// labels, what is enabled and checked, which the pointer or the menu state
// lights); these derive its damage and draw it.
namespace microide::workspace {

// The surface a popup's units are on.
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

// Derives every popup's damage from `frame.menu_popups`: a popup's card is its
// first unit (one that opens, closes, moves or resizes repaints whole), then one
// unit per row keyed on what the row draws -- its label and accelerator, whether
// it is enabled, checked and lit, or a separator. Every frame, open or not: a
// closed popup has no units, so the frame that closes it repaints where it was.
void CommitMenuPopupUnits(PreparedFrameState& frame, SDL_Renderer* renderer, const render::Theme& theme,
                          std::uint64_t text_metrics, const std::function<void(const SDL_FRect&)>& damage);

// Draws `frame.menu_popups` and nothing else, in paint order, each card and row
// inside its own render::PaintElement.
void PaintMenuPopups(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                     const render::TextRenderer& text_renderer);

}  // namespace microide::workspace
