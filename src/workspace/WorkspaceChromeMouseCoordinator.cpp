#include "workspace/WorkspaceChromeMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceMenuCoordinator.h"

namespace microide::workspace {

ChromeMouseCoordinator::ChromeMouseCoordinator(ProjectWorkspaceState& state,
                                               MenuSurfaceState& menu_state,
                                               InteractionState& interaction_state,
                                               Operations operations)
    : state_(state),
      menu_state_(menu_state),
      interaction_state_(interaction_state),
      operations_(std::move(operations)) {}

bool ChromeMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                              const WorkspaceLayout& layout) {
  if (HandleTreeContextMenuButtonDown(event)) {
    return true;
  }

  if (menu_state_.menu_bar_open && event.button.button != SDL_BUTTON_LEFT) {
    operations_.close_menu_bar();
  }

  if (event.button.button == SDL_BUTTON_LEFT && state_.sidebar.visible) {
    const SDL_FRect sidebar_mode_rect = operations_.sidebar_mode_control_rect(layout.sidebar);
    if (Contains(sidebar_mode_rect, event.button.x, event.button.y)) {
      if (menu_state_.menu_bar_open && menu_state_.active_menu_id == MenuId::SidebarMode &&
          menu_state_.active_menu_anchor_rect.has_value()) {
        operations_.close_menu_bar();
      } else {
        operations_.open_anchored_menu(MenuId::SidebarMode, sidebar_mode_rect);
      }
      state_.surface.focus = FocusTarget::Sidebar;
      operations_.request_chrome_redraw();
      return true;
    }
  }

  if (HandleMenuButtonDown(event, layout)) {
    return true;
  }

  if (HandleOverlayButtonDown(event, layout)) {
    return true;
  }

  return false;
}

bool ChromeMouseCoordinator::HandleMotion(const SDL_Event& event, const WorkspaceLayout& layout) {
  if (HandleTreeContextMenuMotion(event)) {
    return true;
  }

  return HandleMenuMotion(event, layout);
}

bool ChromeMouseCoordinator::HandleWheel(const SDL_Event& event,
                                         const WorkspaceLayout& layout,
                                         int vertical_ticks,
                                         int horizontal_ticks) {
  (void)event;
  (void)layout;

  if (!state_.overlay.visible) {
    return false;
  }

  const int overlay_ticks = vertical_ticks != 0 ? vertical_ticks : horizontal_ticks;
  if (state_.overlay.mode == OverlayMode::CommitPicker) {
    operations_.move_compare_picker_selection(-overlay_ticks);
  } else if (state_.overlay.mode == OverlayMode::BufferSearch ||
             state_.overlay.mode == OverlayMode::BufferReplace) {
    operations_.move_buffer_search_selection(-overlay_ticks);
  } else if (state_.overlay.mode == OverlayMode::ProjectSearch) {
    operations_.move_project_search_selection(-overlay_ticks);
  } else {
    operations_.move_file_finder_selection(-overlay_ticks);
  }
  operations_.request_overlay_redraw();
  return true;
}

bool ChromeMouseCoordinator::HandleMenuButtonDown(const SDL_Event& event,
                                                  const WorkspaceLayout& layout) {
  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }

  const auto menu_bar_items = operations_.compute_visible_menu_bar_items(layout.menu_bar);
  const auto window_buttons = operations_.compute_visible_window_control_buttons(layout.menu_bar);
  for (const auto& button : window_buttons) {
    if (!Contains(button.rect, event.button.x, event.button.y)) {
      continue;
    }
    operations_.close_menu_bar();
    switch (button.id) {
      case WorkspaceShell::WindowControlButtonId::Minimize:
        operations_.set_pending_window_action(WorkspaceShell::WindowAction::Minimize);
        break;
      case WorkspaceShell::WindowControlButtonId::Maximize:
        operations_.set_pending_window_action(WorkspaceShell::WindowAction::ToggleMaximize);
        break;
      case WorkspaceShell::WindowControlButtonId::Close:
        operations_.request_quit();
        break;
    }
    operations_.request_chrome_redraw();
    return true;
  }

  if (menu_state_.menu_bar_open) {
    for (const auto& item : menu_bar_items) {
      if (!Contains(item.rect, event.button.x, event.button.y)) {
        continue;
      }
      if (item.id == menu_state_.active_menu_id) {
        operations_.close_menu_bar();
      } else {
        operations_.open_menu_bar_menu(item.id);
      }
      operations_.request_chrome_redraw();
      return true;
    }

    if (const auto submenu_rect = operations_.active_submenu_rect(layout.menu_bar);
        submenu_rect.has_value() && Contains(*submenu_rect, event.button.x, event.button.y)) {
      for (const auto& item :
           operations_.compute_visible_popup_menu_items(menu_state_.active_submenu_id, *submenu_rect)) {
        if (!Contains(item.rect, event.button.x, event.button.y)) {
          continue;
        }
        menu_state_.active_submenu_item_index = item.enabled ? static_cast<int>(item.index) : -1;
        if (!item.separator && item.enabled) {
          operations_.execute_menu_item(menu_state_.active_submenu_id, item.index);
        }
        operations_.request_chrome_redraw();
        return true;
      }
      operations_.request_chrome_redraw();
      return true;
    }

    if (const auto popup_rect =
            operations_.compute_popup_menu_rect(layout.menu_bar, menu_state_.active_menu_id);
        popup_rect.has_value() && Contains(*popup_rect, event.button.x, event.button.y)) {
      for (const auto& item :
           operations_.compute_visible_popup_menu_items(menu_state_.active_menu_id, *popup_rect)) {
        if (!Contains(item.rect, event.button.x, event.button.y)) {
          continue;
        }
        menu_state_.active_menu_item_index = item.enabled ? static_cast<int>(item.index) : -1;
        if (!item.separator && item.enabled) {
          operations_.execute_menu_item(menu_state_.active_menu_id, item.index);
        }
        operations_.request_chrome_redraw();
        return true;
      }
      operations_.request_chrome_redraw();
      return true;
    }

    operations_.close_menu_bar();
    operations_.request_chrome_redraw();
    return true;
  }

  if (!Contains(layout.menu_bar, event.button.x, event.button.y)) {
    return false;
  }

  for (const auto& item : menu_bar_items) {
    if (Contains(item.rect, event.button.x, event.button.y)) {
      operations_.open_menu_bar_menu(item.id);
      operations_.request_chrome_redraw();
      return true;
    }
  }
  if (event.button.clicks >= 2) {
    operations_.set_pending_window_action(WorkspaceShell::WindowAction::ToggleMaximize);
  }
  operations_.request_chrome_redraw();
  return true;
}

bool ChromeMouseCoordinator::HandleMenuMotion(const SDL_Event& event,
                                              const WorkspaceLayout& layout) {
  if (!menu_state_.menu_bar_open) {
    return false;
  }

  for (const auto& item : operations_.compute_visible_menu_bar_items(layout.menu_bar)) {
    if (!Contains(item.rect, event.motion.x, event.motion.y)) {
      continue;
    }
    if (item.id != menu_state_.active_menu_id) {
      operations_.open_menu_bar_menu(item.id);
    }
    operations_.request_chrome_redraw();
    return true;
  }

  if (const auto submenu_rect = operations_.active_submenu_rect(layout.menu_bar);
      submenu_rect.has_value() && Contains(*submenu_rect, event.motion.x, event.motion.y)) {
    menu_state_.active_submenu_item_index = -1;
    for (const auto& item :
         operations_.compute_visible_popup_menu_items(menu_state_.active_submenu_id, *submenu_rect)) {
      if (Contains(item.rect, event.motion.x, event.motion.y)) {
        menu_state_.active_submenu_item_index = item.enabled ? static_cast<int>(item.index) : -1;
        break;
      }
    }
    operations_.request_chrome_redraw();
    return true;
  }

  if (const auto popup_rect =
          operations_.compute_popup_menu_rect(layout.menu_bar, menu_state_.active_menu_id);
      popup_rect.has_value() && Contains(*popup_rect, event.motion.x, event.motion.y)) {
    menu_state_.active_menu_item_index = -1;
    for (const auto& item :
         operations_.compute_visible_popup_menu_items(menu_state_.active_menu_id, *popup_rect)) {
      if (Contains(item.rect, event.motion.x, event.motion.y)) {
        menu_state_.active_menu_item_index = item.enabled ? static_cast<int>(item.index) : -1;
        const MenuSpec* menu = operations_.find_menu_spec(menu_state_.active_menu_id);
        if (menu != nullptr && item.enabled) {
          const auto items = operations_.menu_items(menu_state_.active_menu_id);
          const MenuItemSpec& spec = items[item.index];
          if (spec.submenu != MenuId::None) {
            operations_.open_submenu(spec.submenu, item.rect);
          } else {
            operations_.close_submenu();
          }
        } else {
          operations_.close_submenu();
        }
        break;
      }
    }
    operations_.request_chrome_redraw();
    return true;
  }

  menu_state_.active_menu_item_index = -1;
  operations_.request_chrome_redraw();
  return true;
}

bool ChromeMouseCoordinator::HandleOverlayButtonDown(const SDL_Event& event,
                                                     const WorkspaceLayout& layout) {
  if (!state_.overlay.visible || event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }

  const SDL_FRect overlay = operations_.compute_overlay_rect(layout.editor_area);
  if (!Contains(overlay, event.button.x, event.button.y)) {
    operations_.dismiss_overlay(false);
    operations_.request_overlay_redraw();
    return true;
  }

  operations_.clamp_overlay_scroll_row(overlay);
  const auto list_layout = operations_.compute_overlay_list_layout(overlay);
  if (list_layout.scrollbar.has_value() &&
      Contains(list_layout.scrollbar->track, event.button.x, event.button.y)) {
    interaction_state_.drag_target = DragTarget::OverlayScrollbar;
    interaction_state_.drag_scrollbar_offset =
        Contains(list_layout.scrollbar->thumb, event.button.x, event.button.y)
            ? static_cast<float>(event.button.y) - list_layout.scrollbar->thumb.y
            : list_layout.scrollbar->thumb.h * 0.5f;
    state_.overlay.scroll_row = std::clamp(
        static_cast<int>(std::lround(ScrollUnitsForPointer(
            *list_layout.scrollbar, static_cast<float>(event.button.y),
            interaction_state_.drag_scrollbar_offset))),
        0, list_layout.max_scroll);
    state_.surface.focus = FocusTarget::Overlay;
    operations_.request_overlay_redraw();
    return true;
  }

  if (const auto item_index =
          ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
      item_index.has_value() && *item_index >= 0 &&
      *item_index < static_cast<int>(operations_.overlay_item_count())) {
    operations_.set_overlay_selected_index(static_cast<std::size_t>(*item_index));
    operations_.reveal_overlay_selection(overlay);
    if (state_.overlay.mode == OverlayMode::CommitPicker) {
      operations_.activate_overlay_selection();
    }
  }
  state_.surface.focus = FocusTarget::Overlay;
  operations_.request_overlay_redraw();
  return true;
}

bool ChromeMouseCoordinator::HandleTreeContextMenuButtonDown(const SDL_Event& event) {
  if (!menu_state_.tree_context_menu.open) {
    return false;
  }

  if (const auto popup_rect = operations_.compute_tree_context_menu_rect();
      popup_rect.has_value() && Contains(*popup_rect, event.button.x, event.button.y)) {
    for (const auto& item : operations_.compute_visible_tree_context_menu_items(
             menu_state_.tree_context_menu.target, menu_state_.tree_context_menu.active_item_index,
             *popup_rect)) {
      if (!Contains(item.rect, event.button.x, event.button.y)) {
        continue;
      }
      menu_state_.tree_context_menu.active_item_index =
          item.enabled ? static_cast<int>(item.index) : -1;
      if (event.button.button == SDL_BUTTON_LEFT && !item.separator && item.enabled) {
        operations_.execute_tree_context_menu_item(item.index);
      }
      operations_.request_chrome_redraw();
      return true;
    }
    operations_.request_chrome_redraw();
    return true;
  }

  operations_.close_tree_context_menu();
  operations_.request_chrome_redraw();
  return false;
}

bool ChromeMouseCoordinator::HandleTreeContextMenuMotion(const SDL_Event& event) {
  if (!menu_state_.tree_context_menu.open) {
    return false;
  }

  if (const auto popup_rect = operations_.compute_tree_context_menu_rect();
      popup_rect.has_value() && Contains(*popup_rect, event.motion.x, event.motion.y)) {
    menu_state_.tree_context_menu.active_item_index = -1;
    for (const auto& item : operations_.compute_visible_tree_context_menu_items(
             menu_state_.tree_context_menu.target, menu_state_.tree_context_menu.active_item_index,
             *popup_rect)) {
      if (Contains(item.rect, event.motion.x, event.motion.y)) {
        menu_state_.tree_context_menu.active_item_index =
            item.enabled ? static_cast<int>(item.index) : -1;
        break;
      }
    }
    operations_.request_chrome_redraw();
    return true;
  }

  menu_state_.tree_context_menu.active_item_index = -1;
  operations_.request_chrome_redraw();
  return true;
}

ChromeMouseCoordinator WorkspaceShell::MakeChromeMouseCoordinator() {
  return ChromeMouseCoordinator(
      context_.current_project_state, context_.menu_state, context_.interaction_state,
      ChromeMouseCoordinator::Operations{
          .close_menu_bar = [this]() { MakeMenuCoordinator().CloseMenuBar(); },
          .open_anchored_menu =
              [this](MenuId id, const SDL_FRect& anchor_rect) {
                MakeMenuCoordinator().OpenAnchoredMenu(id, anchor_rect);
              },
          .sidebar_mode_control_rect =
              [this](const SDL_FRect& rect) { return SidebarModeControlRect(rect); },
          .request_chrome_redraw = [this]() { RequestChromeRedraw(); },
          .compute_visible_menu_bar_items =
              [this](const SDL_FRect& rect) { return ComputeVisibleMenuBarItems(rect); },
          .compute_visible_window_control_buttons =
              [this](const SDL_FRect& rect) { return ComputeVisibleWindowControlButtons(rect); },
          .set_pending_window_action =
              [this](WorkspaceShell::WindowAction action) { pending_window_action_ = action; },
          .request_quit = [this]() { RequestQuit(); },
          .open_menu_bar_menu = [this](MenuId id) { MakeMenuCoordinator().OpenMenuBarMenu(id); },
          .active_submenu_rect = [this](const SDL_FRect& rect) { return ActiveSubmenuRect(rect); },
          .compute_visible_popup_menu_items =
              [this](MenuId id, const SDL_FRect& rect) {
                return ComputeVisiblePopupMenuItems(id, rect);
              },
          .execute_menu_item =
              [this](MenuId id, std::size_t item_index) {
                return MakeMenuCoordinator().ExecuteMenuItem(id, item_index);
              },
          .compute_popup_menu_rect =
              [this](const SDL_FRect& menu_bar, MenuId id) {
                return ComputePopupMenuRect(menu_bar, id);
              },
          .find_menu_spec = [](MenuId id) { return WorkspaceShell::FindMenuSpec(id); },
          .menu_items = [this](MenuId id) { return MenuItems(id); },
          .open_submenu =
              [this](MenuId id, const SDL_FRect& rect) {
                MakeMenuCoordinator().OpenSubmenu(id, rect);
              },
          .close_submenu = [this]() { MakeMenuCoordinator().CloseSubmenu(); },
          .move_compare_picker_selection = [this](int delta) { MoveComparePickerSelection(delta); },
          .move_buffer_search_selection = [this](int delta) { MoveBufferSearchSelection(delta); },
          .move_project_search_selection = [this](int delta) { MoveProjectSearchSelection(delta); },
          .move_file_finder_selection = [this](int delta) { MoveFileFinderSelection(delta); },
          .request_overlay_redraw = [this]() { RequestOverlayRedraw(); },
          .dismiss_overlay = [this](bool focus_editor) { DismissOverlay(focus_editor); },
          .compute_overlay_rect = [this](const SDL_FRect& rect) { return ComputeOverlayRect(rect); },
          .clamp_overlay_scroll_row = [this](const SDL_FRect& rect) { ClampOverlayScrollRow(rect); },
          .compute_overlay_list_layout =
              [this](const SDL_FRect& rect) { return ComputeOverlayListLayout(rect); },
          .overlay_item_count = [this]() { return OverlayItemCount(); },
          .set_overlay_selected_index = [this](std::size_t index) { SetOverlaySelectedIndex(index); },
          .reveal_overlay_selection = [this](const SDL_FRect& rect) { RevealOverlaySelection(rect); },
          .activate_overlay_selection = [this]() { ActivateOverlaySelection(); },
          .compute_tree_context_menu_rect = [this]() { return ComputeTreeContextMenuRect(); },
          .compute_visible_tree_context_menu_items =
              [this](TreeContextTargetKind target, int active_item_index, const SDL_FRect& rect) {
                return ComputeVisiblePopupMenuItems(TreeContextMenuItems(target), active_item_index, rect);
              },
          .execute_tree_context_menu_item =
              [this](std::size_t index) {
                return MakeMenuCoordinator().ExecuteTreeContextMenuItem(index);
              },
          .close_tree_context_menu = [this]() { MakeMenuCoordinator().CloseTreeContextMenu(); },
      });
}

}  // namespace microide::workspace
