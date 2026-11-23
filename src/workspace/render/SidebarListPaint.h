#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <functional>
#include <string_view>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/render/PreparedFrameState.h"

// The sidebar's shared two-column list -- Problems, Tests, Outline and the plugin
// item views -- as a derived-damage surface (openspec derived-damage 5.3), painted
// element by element. Free functions, as the file tree's (TreeSidebarPaint.h).
namespace microide::workspace {

// One entry of the list the view on screen shows, read by prep. The labels are
// copied into the frame's snapshot, so they need only outlive the call.
struct SidebarListRow {
  std::string_view primary;
  std::string_view secondary;
  int depth = 0;
  bool collapsible = false;
  bool collapsed = false;
};

// An empty list's note: under the list's first row, down to the list's bottom.
SDL_FRect SidebarPlaceholderBox(const SDL_FRect& sidebar, const ScrollableListLayout& list);

struct SidebarListInputs {
  SDL_Renderer* renderer = nullptr;  // for the device-pixel snap of unit rects
  bool visible = false;              // the sidebar is on screen showing one of these views
  SDL_FRect sidebar{};
  ScrollableListLayout list;  // laid out for this frame from the stored scroll
  std::size_t row_count = 0;
  std::size_t selected_index = 0;
  std::function<SidebarListRow(std::size_t)> row_at;
  std::string_view placeholder;  // drawn when non-empty (an empty list's note)
  bool placeholder_is_error = false;
  bool scrollbar_active = false;  // the thumb is being dragged
  const render::Theme* theme = nullptr;
  const render::TextRenderer* text_renderer = nullptr;
  std::function<bool(const SDL_FRect&)> pointer_over;
  std::function<void(const SDL_FRect&)> damage;
};

// Lays the list out into `frame.sidebar_list` and derives its damage: the
// placeholder, the scrollbar and each visible row are units keyed on what they
// draw, the row's hover decided here as paint used to decide it.
void PrepareSidebarListFrame(PreparedFrameState& frame, const SidebarListInputs& inputs);

// Draws `frame.sidebar_list` and nothing else, each element inside its own
// render::PaintElement.
void PaintSidebarList(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                      const render::TextRenderer& text_renderer);

}  // namespace microide::workspace
