#pragma once

#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "workspace/WorkspaceLayout.h"
#include "workspace/state/WorkspaceMenuState.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspaceInteractionState.h"
#include "workspace/shell/WorkspaceShell.h"

namespace microide::workspace {

class MenuCoordinator;

class ChromeMouseCoordinator {
 public:
  struct Services {
    MenuCoordinator& menus;
  };

  struct Operations {
    std::function<SidebarModeRowLayout(const SDL_FRect&)> sidebar_mode_row;
    std::function<void(std::string_view)> activate_sidebar_view;
    std::function<void()> request_frame;
    std::function<WorkspaceShell::VisibleMenuBarItems(const SDL_FRect&)>
        compute_visible_menu_bar_items;
    std::function<WorkspaceShell::MenuBarOverflowIds(const SDL_FRect&)>
        compute_overflow_menu_bar_items;
    std::function<std::optional<SDL_FRect>(const SDL_FRect&)> menu_overflow_chevron_rect;
    std::function<WorkspaceShell::VisibleWindowControlButtons(const SDL_FRect&)>
        compute_visible_window_control_buttons;
    std::function<void(WorkspaceShell::WindowAction)> set_pending_window_action;
    std::function<void()> request_quit;
    std::function<std::optional<SDL_FRect>(const SDL_FRect&)> active_submenu_rect;
    std::function<std::optional<PopupRowGeometry>(
        std::span<const MenuItemSpec>, const SDL_FRect&, float, float)>
        hit_test_popup_row;
    std::function<bool(const MenuItemSpec&)> is_menu_item_enabled;
    std::function<std::optional<SDL_FRect>(const SDL_FRect&, MenuId)> compute_popup_menu_rect;
    std::function<const MenuSpec*(MenuId)> find_menu_spec;
    std::function<std::span<const MenuItemSpec>(MenuId)> menu_items;
    std::function<void(int)> move_buffer_search_selection;
    std::function<void()> replace_current_buffer_search_match;
    std::function<void()> replace_all_buffer_search_matches;
    std::function<void(BufferFindToggle)> toggle_buffer_search_option;
    std::function<void()> request_overlay_redraw;
    std::function<void(bool)> dismiss_overlay;
    std::function<SDL_FRect(const SDL_FRect&)> compute_overlay_rect;
    std::function<void(const SDL_FRect&)> clamp_overlay_scroll_row;
    std::function<ScrollableListLayout(const SDL_FRect&)> compute_overlay_list_layout;
    std::function<std::size_t()> overlay_item_count;
    std::function<void(std::size_t)> set_overlay_selected_index;
    std::function<void(const SDL_FRect&)> reveal_overlay_selection;
    std::function<void()> activate_overlay_selection;
    std::function<std::optional<SDL_FRect>()> compute_tree_context_menu_rect;
    std::function<std::optional<PopupRowGeometry>(
        TreeContextTargetKind, const SDL_FRect&, float, float)>
        hit_test_tree_context_menu_row;
    std::function<bool(TreeContextTargetKind, std::size_t)> is_tree_context_menu_item_enabled_at;
  };

  ChromeMouseCoordinator(ProjectWorkspaceState& state,
                         MenuSurfaceState& menu_state,
                         InteractionState& interaction_state,
                         Services services,
                         Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks,
                   int horizontal_ticks);

 private:
  struct PopupRowPair {
    int hovered = -1;
    int active = -1;
  };

  bool HandleMenuButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleMenuMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  struct PopupRowHover {
    int index = -1;
    bool enabled = false;
    SDL_FRect rect{};
  };
  PopupRowHover HoveredPopupRow(std::span<const MenuItemSpec> items, const SDL_FRect& popup_rect,
                                float x, float y) const;
  void RequestFrameIfPopupRowsChanged(PopupRowPair previous, PopupRowPair current);
  bool HandleMenuOverflowPopupMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  void CloseOverflowPopupWithRedraw();
  bool HandleOverlayButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleTreeContextMenuButtonDown(const SDL_Event& event);
  bool HandleTreeContextMenuMotion(const SDL_Event& event);

  ProjectWorkspaceState& state_;
  MenuSurfaceState& menu_state_;
  InteractionState& interaction_state_;
  Services services_;
  Operations operations_;
};

}
