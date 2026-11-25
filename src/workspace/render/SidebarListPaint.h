#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <functional>
#include <string_view>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/render/PreparedFrameState.h"

namespace microide::workspace {

struct SidebarListRow {
  std::string_view primary;
  std::string_view secondary;
  int depth = 0;
  bool collapsible = false;
  bool collapsed = false;
};

SDL_FRect SidebarPlaceholderBox(const SDL_FRect& sidebar, const ScrollableListLayout& list);

struct SidebarListInputs {
  SDL_Renderer* renderer = nullptr;
  bool visible = false;
  SDL_FRect sidebar{};
  ScrollableListLayout list;
  std::size_t row_count = 0;
  std::size_t selected_index = 0;
  std::function<SidebarListRow(std::size_t)> row_at;
  std::string_view placeholder;
  bool placeholder_is_error = false;
  bool scrollbar_active = false;
  const render::Theme* theme = nullptr;
  const render::TextRenderer* text_renderer = nullptr;
  std::function<bool(const SDL_FRect&)> pointer_over;
  std::function<void(const SDL_FRect&)> damage;
};

void PrepareSidebarListFrame(PreparedFrameState& frame, const SidebarListInputs& inputs);

void PaintSidebarList(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                      const render::TextRenderer& text_renderer);

}
