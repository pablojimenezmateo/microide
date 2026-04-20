#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <optional>

#include "workspace/WorkspaceMenuRegistry.h"

namespace microide::workspace {

struct TreeContextMenuState {
  bool open = false;
  TreeContextTargetKind target = TreeContextTargetKind::None;
  std::filesystem::path path;
  SDL_FRect anchor_rect{};
  int active_item_index = -1;
};

struct MenuSurfaceState {
  bool menu_bar_open = false;
  MenuId active_menu_id = MenuId::None;
  int active_menu_item_index = -1;
  MenuId active_submenu_id = MenuId::None;
  int active_submenu_item_index = -1;
  std::optional<SDL_FRect> active_menu_anchor_rect;
  std::optional<SDL_FRect> active_submenu_anchor_rect;
  TreeContextMenuState tree_context_menu;
};

}  // namespace microide::workspace
