#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <string_view>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/render/DerivedDamage.h"
#include "workspace/render/PreparedFrameState.h"
#include "workspace/state/WorkspaceSidebarState.h"

// The sidebar's mode tab row -- Project / Search / Source Control and the overflow
// button for the other views -- as a derived-damage surface, and the one place
// that says which sidebar views derive ALL they show.
namespace microide::workspace {

// The sidebar views whose every piece on screen derives its damage: the mode row
// (below) and the view's own surface. For such a view "the sidebar changed" owes
// a frame, not the sidebar's rect (WorkspaceShell::RequestSidebarRedraw): prep
// repaints what changed. A view joins by deriving its surface and adding itself
// here and to SidebarViewSurface; QaFlowRegression/
// ADerivedSidebarViewCommitsItsSurfacesUnits fails a view named here whose
// surface lays out no units.
constexpr bool SidebarViewIsDerived(SidebarMode mode) { return mode == SidebarMode::Tree; }

// The surface a derived view's own content is laid out on.
constexpr DerivedSurface SidebarViewSurface(SidebarMode mode) {
  return mode == SidebarMode::Tree ? DerivedSurface::SidebarTree : DerivedSurface::Count;
}

struct SidebarModeRowInputs {
  SDL_Renderer* renderer = nullptr;  // for the device-pixel snap of unit rects
  bool visible = false;              // the sidebar is on screen
  SidebarModeRowLayout row;
  std::string_view active_view_id;
  bool overflow_menu_open = false;  // the overflow's menu is up: its button stays lit
  const render::Theme* theme = nullptr;
  const render::TextRenderer* text_renderer = nullptr;
  std::function<bool(const SDL_FRect&)> pointer_over;
  std::function<void(const SDL_FRect&)> damage;
};

// Lays the row out into `frame.sidebar_mode_row` and derives its damage: each tab
// and the overflow button is a unit (a tab that is not laid out keeps its slot).
void PrepareSidebarModeRowFrame(PreparedFrameState& frame, const SidebarModeRowInputs& inputs);

// Draws `frame.sidebar_mode_row` and nothing else, each element inside its own
// render::PaintElement.
void PaintSidebarModeRow(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                         const render::TextRenderer& text_renderer);

}  // namespace microide::workspace
