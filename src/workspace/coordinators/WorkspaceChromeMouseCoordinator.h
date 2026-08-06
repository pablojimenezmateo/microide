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

class ChromeMouseCoordinator {
 public:
  struct Operations {
    std::function<void()> close_menu_bar;
    std::function<void(MenuId, const SDL_FRect&)> open_anchored_menu;
    std::function<SidebarModeRowLayout(const SDL_FRect&)> sidebar_mode_row;
    std::function<void(std::string_view)> activate_sidebar_view;
    std::function<void()> request_chrome_redraw;
    std::function<void(const SDL_FRect&)> request_redraw_rect;
    // Heap-free returns: these three run ~10 times per pointer-motion event over
    // the menu bar, and returning std::vector made that 32 of the 50 allocations
    // an event (TD-2026-08-06-149).
    std::function<WorkspaceShell::VisibleMenuBarItems(const SDL_FRect&)>
        compute_visible_menu_bar_items;
    std::function<WorkspaceShell::MenuBarOverflowIds(const SDL_FRect&)>
        compute_overflow_menu_bar_items;
    std::function<std::optional<SDL_FRect>(const SDL_FRect&)> menu_overflow_chevron_rect;
    std::function<WorkspaceShell::VisibleWindowControlButtons(const SDL_FRect&)>
        compute_visible_window_control_buttons;
    std::function<void(WorkspaceShell::WindowAction)> set_pending_window_action;
    std::function<void()> request_quit;
    std::function<void(MenuId)> open_menu_bar_menu;
    std::function<std::optional<SDL_FRect>(const SDL_FRect&)> active_submenu_rect;
    std::function<std::vector<WorkspaceShell::VisiblePopupMenuItem>(MenuId, const SDL_FRect&)>
        compute_visible_popup_menu_items;
    // Geometry-only hit/rect lookups for mouse motion. The full
    // `compute_visible_popup_menu_items` path probes IsMenuItemEnabled and
    // IsMenuItemChecked per item, which dominated per-motion handler time.
    std::function<std::optional<WorkspaceShell::PopupRowGeometry>(
        MenuId, const SDL_FRect&, float, float)>
        hit_test_popup_row;
    std::function<std::optional<SDL_FRect>(MenuId, const SDL_FRect&, std::size_t)>
        popup_row_rect_by_index;
    std::function<bool(MenuId, std::size_t)> is_menu_item_enabled_at;
    std::function<bool(MenuId, std::size_t)> execute_menu_item;
    std::function<std::optional<SDL_FRect>(const SDL_FRect&, MenuId)> compute_popup_menu_rect;
    std::function<const MenuSpec*(MenuId)> find_menu_spec;
    std::function<std::span<const MenuItemSpec>(MenuId)> menu_items;
    // The find widget is the one overlay whose wheel/buttons still move a selection
    // (they step through matches); every list overlay scrolls detached instead, so no
    // per-mode "move selection" hook is needed here.
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
    std::function<std::vector<WorkspaceShell::VisiblePopupMenuItem>(TreeContextTargetKind,
                                                                    int,
                                                                    const SDL_FRect&)>
        compute_visible_tree_context_menu_items;
    std::function<bool(std::size_t)> execute_tree_context_menu_item;
    std::function<void()> close_tree_context_menu;
  };

  ChromeMouseCoordinator(ProjectWorkspaceState& state,
                         MenuSurfaceState& menu_state,
                         InteractionState& interaction_state,
                         Operations operations);

  bool HandleButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleWheel(const SDL_Event& event,
                   const WorkspaceLayout& layout,
                   int vertical_ticks,
                   int horizontal_ticks);

 private:
  bool HandleMenuButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleMenuMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleOverlayButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleTreeContextMenuButtonDown(const SDL_Event& event);
  bool HandleTreeContextMenuMotion(const SDL_Event& event);

  ProjectWorkspaceState& state_;
  MenuSurfaceState& menu_state_;
  InteractionState& interaction_state_;
  Operations operations_;
};

}  // namespace microide::workspace
