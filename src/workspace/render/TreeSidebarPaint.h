#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "project/DirectoryTree.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/registries/WorkspaceFileIconRegistry.h"
#include "workspace/render/PreparedFrameState.h"

namespace microide::workspace {

struct TreeSidebarInputs {
  SDL_Renderer* renderer = nullptr;
  bool visible = false;
  std::span<const project::TreeEntry> entries;
  std::size_t selected_index = 0;
  ScrollableListLayout list;
  SDL_FRect collapse_button{};
  SDL_FRect refresh_button{};
  bool can_collapse_all = false;
  const std::vector<std::optional<WorkspaceFileIconRegistry::Icon>>* icons = nullptr;
  bool scrollbar_active = false;
  const render::Theme* theme = nullptr;
  const render::TextRenderer* text_renderer = nullptr;
  std::function<bool(const SDL_FRect&)> pointer_over;
  std::function<void(const SDL_FRect&)> damage;
};

void PrepareTreeSidebarFrame(PreparedFrameState& frame, const TreeSidebarInputs& inputs);

void PaintTreeSidebar(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                      const render::TextRenderer& text_renderer);

}
