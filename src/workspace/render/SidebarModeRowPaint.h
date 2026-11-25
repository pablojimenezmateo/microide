#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <string_view>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/render/DerivedDamage.h"
#include "workspace/render/PreparedFrameState.h"
#include "workspace/state/WorkspaceSidebarState.h"

namespace microide::workspace {

constexpr bool SidebarViewUsesTheList(SidebarMode mode) {
  return UsesPluginItemSurface(mode) || mode == SidebarMode::Problems || mode == SidebarMode::Tests;
}

constexpr DerivedSurface SidebarViewSurface(SidebarMode mode) {
  return mode == SidebarMode::Tree     ? DerivedSurface::SidebarTree
         : mode == SidebarMode::Git    ? DerivedSurface::GitPanel
         : mode == SidebarMode::Search ? DerivedSurface::SidebarSearch
         : SidebarViewUsesTheList(mode) ? DerivedSurface::SidebarList
                                        : DerivedSurface::Count;
}

struct SidebarModeRowInputs {
  SDL_Renderer* renderer = nullptr;
  bool visible = false;
  SidebarModeRowLayout row;
  std::string_view active_view_id;
  bool overflow_menu_open = false;
  const render::Theme* theme = nullptr;
  const render::TextRenderer* text_renderer = nullptr;
  std::function<bool(const SDL_FRect&)> pointer_over;
  std::function<void(const SDL_FRect&)> damage;
};

void PrepareSidebarModeRowFrame(PreparedFrameState& frame, const SidebarModeRowInputs& inputs);

void PaintSidebarModeRow(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                         const render::TextRenderer& text_renderer);

}
