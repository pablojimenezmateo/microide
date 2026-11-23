#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include "project/ProjectSearchService.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/render/PreparedFrameState.h"

// The sidebar's Search panel -- its query, replace and scope fields, its toggle
// and Replace All buttons, the status line, each visible row of the result list,
// the empty list's note and the scrollbar -- as a derived-damage surface painted
// element by element. Free functions, as the file tree's (TreeSidebarPaint.h).
//
// The panel was painted from live state inside the sidebar's paint, which also
// stored the list's clamped scroll, and every change to it -- a key in a field, a
// batch of hits landing from the search's workers, a hover -- named the sidebar's
// rect. Prep lays it out once and copies the text it shows; a change owes a frame.
namespace microide::workspace {

struct SearchSidebarFieldInputs {
  SDL_FRect rect{};  // empty: not laid out
  std::string_view text;
  bool focused = false;
  bool editing = false;
  // The caret and selection the text-input pass draws over the focused field
  // (0: none): the field is repainted when they move.
  std::uint64_t caret_key = 0;
};

struct SearchSidebarInputs {
  SDL_Renderer* renderer = nullptr;  // for the device-pixel snap of unit rects
  bool visible = false;              // the sidebar is on screen showing Search
  std::array<SearchSidebarFieldInputs, 4> fields{};
  // Rect, label, enabled and active; prep decides the hover.
  std::array<SearchSidebarButtonPaint, 5> buttons{};
  SDL_FRect status_rect{};
  std::string_view status;
  // The results and their grouped line map (-1: the header of the file whose
  // matches follow), with the list laid out for this frame from the stored scroll.
  std::span<const project::ProjectSearchResult> results;
  std::span<const int> line_map;
  std::size_t selected_index = 0;
  ScrollableListLayout list;
  SDL_FRect placeholder_rect{};
  std::string_view empty_text;
  bool empty_is_error = false;
  bool scrollbar_active = false;  // the thumb is being dragged
  const render::Theme* theme = nullptr;
  const render::TextRenderer* text_renderer = nullptr;
  std::function<bool(const SDL_FRect&)> pointer_over;
  std::function<void(const SDL_FRect&)> damage;
};

// Lays the panel out into `frame.search_sidebar` and derives its damage: each
// field, button and row, the status line, the note and the scrollbar is a unit
// keyed on what it draws.
void PrepareSearchSidebarFrame(PreparedFrameState& frame, const SearchSidebarInputs& inputs);

// Draws `frame.search_sidebar` and nothing else, each element inside its own
// render::PaintElement.
void PaintSearchSidebar(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                        const render::TextRenderer& text_renderer);

}  // namespace microide::workspace
