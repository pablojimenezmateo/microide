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

namespace microide::workspace {

struct SearchSidebarFieldInputs {
  SDL_FRect rect{};
  std::string_view text;
  bool focused = false;
  bool editing = false;
  std::uint64_t caret_key = 0;
};

struct SearchSidebarInputs {
  SDL_Renderer* renderer = nullptr;
  bool visible = false;
  std::array<SearchSidebarFieldInputs, 4> fields{};
  std::array<SearchSidebarButtonPaint, 5> buttons{};
  SDL_FRect status_rect{};
  std::string_view status;
  std::span<const project::ProjectSearchResult> results;
  std::span<const int> line_map;
  std::size_t selected_index = 0;
  ScrollableListLayout list;
  SDL_FRect placeholder_rect{};
  std::string_view empty_text;
  bool empty_is_error = false;
  bool scrollbar_active = false;
  const render::Theme* theme = nullptr;
  const render::TextRenderer* text_renderer = nullptr;
  std::function<bool(const SDL_FRect&)> pointer_over;
  std::function<void(const SDL_FRect&)> damage;
};

void PrepareSearchSidebarFrame(PreparedFrameState& frame, const SearchSidebarInputs& inputs);

void PaintSearchSidebar(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                        const render::TextRenderer& text_renderer);

}
