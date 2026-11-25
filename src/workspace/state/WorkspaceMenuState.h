#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

#include "workspace/registries/WorkspaceMenuRegistry.h"

namespace microide::workspace {

struct TreeContextMenuState {
  bool open = false;
  TreeContextTargetKind target = TreeContextTargetKind::None;
  std::filesystem::path path;
  std::size_t line = 0;
  bool breakpoint_enabled = true;
  SDL_FRect anchor_rect{};
  int active_item_index = -1;
};

inline std::filesystem::path ReviewCommentMenuScope(const TreeContextMenuState& menu) {
  return menu.open && (menu.target == TreeContextTargetKind::GitEntry ||
                       menu.target == TreeContextTargetKind::GitReviewScope)
             ? menu.path
             : std::filesystem::path();
}

struct MenuSurfaceState {
  bool menu_bar_open = false;
  MenuId active_menu_id = MenuId::None;
  int active_menu_item_index = -1;
  MenuId active_submenu_id = MenuId::None;
  int active_submenu_item_index = -1;
  std::optional<SDL_FRect> active_menu_anchor_rect;
  std::optional<SDL_FRect> active_submenu_anchor_rect;
  int hovered_popup_row_index = -1;
  int hovered_submenu_row_index = -1;
  bool overflow_popup_open = false;
  bool chevron_hovered = false;
  int overflow_popup_active_index = -1;
  TreeContextMenuState tree_context_menu;
  MenuId last_run_menu_id = MenuId::None;
  std::size_t last_run_item_index = 0;
  std::uint64_t rows_run = 0;
};

inline void OpenMenuOverflowPopup(MenuSurfaceState& menu_state) {
  menu_state.overflow_popup_open = true;
  menu_state.overflow_popup_active_index = -1;
}

inline void CloseMenuOverflowPopup(MenuSurfaceState& menu_state) {
  menu_state.overflow_popup_open = false;
  menu_state.overflow_popup_active_index = -1;
}

}
