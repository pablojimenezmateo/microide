#include "workspace/WorkspaceChromeMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

WorkspaceShell::ChromeMouseCoordinator::ChromeMouseCoordinator(WorkspaceShell& shell)
    : shell_(shell) {}

bool WorkspaceShell::ChromeMouseCoordinator::HandleButtonDown(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  if (HandleTreeContextMenuButtonDown(event)) {
    return true;
  }

  if (shell_.surface_.menu_bar_open && event.button.button != SDL_BUTTON_LEFT) {
    shell_.CloseMenuBar();
  }

  if (event.button.button == SDL_BUTTON_LEFT && shell_.surface_.sidebar_visible) {
    const SDL_FRect sidebar_mode_rect = shell_.SidebarModeControlRect(layout.sidebar);
    if (Contains(sidebar_mode_rect, event.button.x, event.button.y)) {
      if (shell_.surface_.menu_bar_open &&
          shell_.surface_.active_menu_id == MenuId::SidebarMode &&
          shell_.surface_.active_menu_anchor_rect.has_value()) {
        shell_.CloseMenuBar();
      } else {
        shell_.OpenAnchoredMenu(MenuId::SidebarMode, sidebar_mode_rect);
      }
      shell_.surface_.focus = FocusTarget::Sidebar;
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

bool WorkspaceShell::ChromeMouseCoordinator::HandleMotion(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  if (HandleTreeContextMenuMotion(event)) {
    return true;
  }

  return HandleMenuMotion(event, layout);
}

bool WorkspaceShell::ChromeMouseCoordinator::HandleWheel(const SDL_Event& event,
                                                         const WorkspaceLayout& layout,
                                                         int vertical_ticks,
                                                         int horizontal_ticks) {
  (void)event;
  (void)layout;

  if (!shell_.surface_.overlay_visible) {
    return false;
  }

  const int overlay_ticks = vertical_ticks != 0 ? vertical_ticks : horizontal_ticks;
  if (shell_.surface_.overlay_mode == OverlayMode::CommitPicker) {
    shell_.MoveComparePickerSelection(-overlay_ticks);
  } else if (shell_.surface_.overlay_mode == OverlayMode::BufferSearch ||
             shell_.surface_.overlay_mode == OverlayMode::BufferReplace) {
    shell_.MoveBufferSearchSelection(-overlay_ticks);
  } else if (shell_.surface_.overlay_mode == OverlayMode::ProjectSearch) {
    shell_.MoveProjectSearchSelection(-overlay_ticks);
  } else {
    shell_.MoveFileFinderSelection(-overlay_ticks);
  }
  return true;
}

bool WorkspaceShell::ChromeMouseCoordinator::HandleMenuButtonDown(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }

  const auto menu_bar_items = shell_.ComputeVisibleMenuBarItems(layout.menu_bar);
  const auto window_buttons =
      shell_.ComputeVisibleWindowControlButtons(layout.menu_bar);
  for (const VisibleWindowControlButton& button : window_buttons) {
    if (!Contains(button.rect, event.button.x, event.button.y)) {
      continue;
    }
    shell_.CloseMenuBar();
    switch (button.id) {
      case WindowControlButtonId::Minimize:
        shell_.pending_window_action_ = WindowAction::Minimize;
        break;
      case WindowControlButtonId::Maximize:
        shell_.pending_window_action_ = WindowAction::ToggleMaximize;
        break;
      case WindowControlButtonId::Close:
        shell_.RequestQuit();
        break;
    }
    return true;
  }

  if (shell_.surface_.menu_bar_open) {
    for (const VisibleMenuBarItem& item : menu_bar_items) {
      if (!Contains(item.rect, event.button.x, event.button.y)) {
        continue;
      }
      if (item.id == shell_.surface_.active_menu_id) {
        shell_.CloseMenuBar();
      } else {
        shell_.OpenMenuBarMenu(item.id);
      }
      return true;
    }

    if (const auto submenu_rect = shell_.ActiveSubmenuRect(layout.menu_bar);
        submenu_rect.has_value() &&
        Contains(*submenu_rect, event.button.x, event.button.y)) {
      for (const VisiblePopupMenuItem& item :
           shell_.ComputeVisiblePopupMenuItems(shell_.surface_.active_submenu_id,
                                              *submenu_rect)) {
        if (!Contains(item.rect, event.button.x, event.button.y)) {
          continue;
        }
        shell_.surface_.active_submenu_item_index =
            item.enabled ? static_cast<int>(item.index) : -1;
        if (!item.separator && item.enabled) {
          shell_.ExecuteMenuItem(shell_.surface_.active_submenu_id, item.index);
        }
        return true;
      }
      return true;
    }

    if (const auto popup_rect =
            shell_.ComputePopupMenuRect(layout.menu_bar, shell_.surface_.active_menu_id);
        popup_rect.has_value() && Contains(*popup_rect, event.button.x, event.button.y)) {
      for (const VisiblePopupMenuItem& item :
           shell_.ComputeVisiblePopupMenuItems(shell_.surface_.active_menu_id,
                                              *popup_rect)) {
        if (!Contains(item.rect, event.button.x, event.button.y)) {
          continue;
        }
        shell_.surface_.active_menu_item_index =
            item.enabled ? static_cast<int>(item.index) : -1;
        if (!item.separator && item.enabled) {
          shell_.ExecuteMenuItem(shell_.surface_.active_menu_id, item.index);
        }
        return true;
      }
      return true;
    }

    shell_.CloseMenuBar();
    return true;
  }

  if (!Contains(layout.menu_bar, event.button.x, event.button.y)) {
    return false;
  }

  for (const VisibleMenuBarItem& item : menu_bar_items) {
    if (Contains(item.rect, event.button.x, event.button.y)) {
      shell_.OpenMenuBarMenu(item.id);
      return true;
    }
  }
  return true;
}

bool WorkspaceShell::ChromeMouseCoordinator::HandleMenuMotion(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  if (!shell_.surface_.menu_bar_open) {
    return false;
  }

  for (const VisibleMenuBarItem& item :
       shell_.ComputeVisibleMenuBarItems(layout.menu_bar)) {
    if (!Contains(item.rect, event.motion.x, event.motion.y)) {
      continue;
    }
    if (item.id != shell_.surface_.active_menu_id) {
      shell_.OpenMenuBarMenu(item.id);
    }
    return true;
  }

  if (const auto submenu_rect = shell_.ActiveSubmenuRect(layout.menu_bar);
      submenu_rect.has_value() && Contains(*submenu_rect, event.motion.x, event.motion.y)) {
    shell_.surface_.active_submenu_item_index = -1;
    for (const VisiblePopupMenuItem& item :
         shell_.ComputeVisiblePopupMenuItems(shell_.surface_.active_submenu_id,
                                            *submenu_rect)) {
      if (Contains(item.rect, event.motion.x, event.motion.y)) {
        shell_.surface_.active_submenu_item_index =
            item.enabled ? static_cast<int>(item.index) : -1;
        break;
      }
    }
    return true;
  }

  if (const auto popup_rect =
          shell_.ComputePopupMenuRect(layout.menu_bar, shell_.surface_.active_menu_id);
      popup_rect.has_value() && Contains(*popup_rect, event.motion.x, event.motion.y)) {
    shell_.surface_.active_menu_item_index = -1;
    for (const VisiblePopupMenuItem& item :
         shell_.ComputeVisiblePopupMenuItems(shell_.surface_.active_menu_id, *popup_rect)) {
      if (Contains(item.rect, event.motion.x, event.motion.y)) {
        shell_.surface_.active_menu_item_index =
            item.enabled ? static_cast<int>(item.index) : -1;
        const MenuSpec* menu = shell_.FindMenuSpec(shell_.surface_.active_menu_id);
        if (menu != nullptr && item.enabled) {
          const MenuItemSpec& spec = menu->items[item.index];
          if (spec.submenu != MenuId::None) {
            shell_.OpenSubmenu(spec.submenu, item.rect);
          } else {
            shell_.CloseSubmenu();
          }
        } else {
          shell_.CloseSubmenu();
        }
        break;
      }
    }
    return true;
  }

  shell_.surface_.active_menu_item_index = -1;
  return true;
}

bool WorkspaceShell::ChromeMouseCoordinator::HandleOverlayButtonDown(
    const SDL_Event& event,
    const WorkspaceLayout& layout) {
  if (!shell_.surface_.overlay_visible || event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }

  const SDL_FRect overlay = shell_.ComputeOverlayRect(layout.editor_area);
  if (!Contains(overlay, event.button.x, event.button.y)) {
    shell_.DismissOverlay();
    return true;
  }

  shell_.ClampOverlayScrollRow(overlay);
  const auto list_layout = shell_.ComputeOverlayListLayout(overlay);
  if (list_layout.scrollbar.has_value() &&
      Contains(list_layout.scrollbar->track, event.button.x, event.button.y)) {
    shell_.surface_.drag_target = DragTarget::OverlayScrollbar;
    shell_.surface_.drag_scrollbar_offset =
        Contains(list_layout.scrollbar->thumb, event.button.x, event.button.y)
            ? static_cast<float>(event.button.y) - list_layout.scrollbar->thumb.y
            : list_layout.scrollbar->thumb.h * 0.5f;
    shell_.surface_.overlay_scroll_row = std::clamp(
        static_cast<int>(std::lround(ScrollUnitsForPointer(
            *list_layout.scrollbar, static_cast<float>(event.button.y),
            shell_.surface_.drag_scrollbar_offset))),
        0, list_layout.max_scroll);
    shell_.surface_.focus = FocusTarget::Overlay;
    return true;
  }

  if (const auto item_index =
          ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
      item_index.has_value() && *item_index >= 0 &&
      *item_index < static_cast<int>(shell_.OverlayItemCount())) {
    shell_.SetOverlaySelectedIndex(static_cast<std::size_t>(*item_index));
    shell_.RevealOverlaySelection(overlay);
    if (shell_.surface_.overlay_mode == OverlayMode::CommitPicker) {
      shell_.ActivateOverlaySelection();
    }
  }
  shell_.surface_.focus = FocusTarget::Overlay;
  return true;
}

bool WorkspaceShell::ChromeMouseCoordinator::HandleTreeContextMenuButtonDown(
    const SDL_Event& event) {
  if (!shell_.surface_.tree_context_menu.open) {
    return false;
  }

  if (const auto popup_rect = shell_.ComputeTreeContextMenuRect();
      popup_rect.has_value() &&
      Contains(*popup_rect, event.button.x, event.button.y)) {
    for (const VisiblePopupMenuItem& item : shell_.ComputeVisiblePopupMenuItems(
             TreeContextMenuItems(shell_.surface_.tree_context_menu.target),
             shell_.surface_.tree_context_menu.active_item_index, *popup_rect)) {
      if (!Contains(item.rect, event.button.x, event.button.y)) {
        continue;
      }
      shell_.surface_.tree_context_menu.active_item_index =
          item.enabled ? static_cast<int>(item.index) : -1;
      if (event.button.button == SDL_BUTTON_LEFT && !item.separator && item.enabled) {
        shell_.ExecuteTreeContextMenuItem(item.index);
      }
      return true;
    }
    return true;
  }

  shell_.CloseTreeContextMenu();
  return false;
}

bool WorkspaceShell::ChromeMouseCoordinator::HandleTreeContextMenuMotion(
    const SDL_Event& event) {
  if (!shell_.surface_.tree_context_menu.open) {
    return false;
  }

  if (const auto popup_rect = shell_.ComputeTreeContextMenuRect();
      popup_rect.has_value() &&
      Contains(*popup_rect, event.motion.x, event.motion.y)) {
    shell_.surface_.tree_context_menu.active_item_index = -1;
    for (const VisiblePopupMenuItem& item : shell_.ComputeVisiblePopupMenuItems(
             TreeContextMenuItems(shell_.surface_.tree_context_menu.target),
             shell_.surface_.tree_context_menu.active_item_index, *popup_rect)) {
      if (Contains(item.rect, event.motion.x, event.motion.y)) {
        shell_.surface_.tree_context_menu.active_item_index =
            item.enabled ? static_cast<int>(item.index) : -1;
        break;
      }
    }
    return true;
  }

  shell_.surface_.tree_context_menu.active_item_index = -1;
  return true;
}

}  // namespace microide::workspace
