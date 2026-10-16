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
  // 0-based buffer line for the BreakpointLine target (unused otherwise).
  std::size_t line = 0;
  // Enabled state of the breakpoint under a BreakpointLine menu, captured when the
  // menu opens; drives the dynamic "Disable"/"Enable Breakpoint" item label.
  bool breakpoint_enabled = true;
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
  // Row currently under the cursor in the popup/submenu, including disabled
  // rows and separators (the enabled-only `active_*_item_index` above drives
  // selection/keyboard navigation). Tracking this lets motion handlers issue
  // a narrow row redraw on every hover transition, including disabled rows.
  int hovered_popup_row_index = -1;
  int hovered_submenu_row_index = -1;
  bool overflow_popup_open = false;
  std::optional<SDL_FRect> overflow_popup_anchor_rect;
  // Keyboard-selected row in the overflow popup, or -1 for none. The popup's
  // hover fill reads this and nothing else: the mouse motion path writes it too,
  // so the pointer and the keyboard can never highlight two different rows.
  int overflow_popup_active_index = -1;
  TreeContextMenuState tree_context_menu;
  // The row a click or Enter last ran, and how many rows have run: the `ui`
  // query's receipt. A press meant for a row can be taken by a surface beneath
  // the menu (the sidebar's view buttons were), and then nothing on screen says
  // which handler had it; a driver compares this with the row it aimed at.
  MenuId last_run_menu_id = MenuId::None;
  std::size_t last_run_item_index = 0;
  std::uint64_t rows_run = 0;
};

// The overflow popup's three fields move together. Four call sites used to reset
// two of them by hand and leave the third, which is how the active index ended up
// written-once and read-nowhere (TD-2026-09-19-296).
inline void OpenMenuOverflowPopup(MenuSurfaceState& menu_state, const SDL_FRect& anchor_rect) {
  menu_state.overflow_popup_open = true;
  menu_state.overflow_popup_anchor_rect = anchor_rect;
  menu_state.overflow_popup_active_index = -1;
}

inline void CloseMenuOverflowPopup(MenuSurfaceState& menu_state) {
  menu_state.overflow_popup_open = false;
  menu_state.overflow_popup_anchor_rect.reset();
  menu_state.overflow_popup_active_index = -1;
}

}  // namespace microide::workspace
