#include "workspace/WorkspaceChromeMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceMenuCoordinator.h"

namespace microide::workspace {

ChromeMouseCoordinator::ChromeMouseCoordinator(WorkspaceShell& shell) : shell_(shell) {}

bool ChromeMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                              const WorkspaceLayout& layout) {
  if (HandleTreeContextMenuButtonDown(event)) {
    return true;
  }

  if (shell_.menu_state_.menu_bar_open && event.button.button != SDL_BUTTON_LEFT) {
    shell_.MakeMenuCoordinator().CloseMenuBar();
  }

  if (event.button.button == SDL_BUTTON_LEFT && shell_.sidebar_state_.visible) {
    const SDL_FRect sidebar_mode_rect = shell_.SidebarModeControlRect(layout.sidebar);
    if (Contains(sidebar_mode_rect, event.button.x, event.button.y)) {
      if (shell_.menu_state_.menu_bar_open &&
          shell_.menu_state_.active_menu_id == MenuId::SidebarMode &&
          shell_.menu_state_.active_menu_anchor_rect.has_value()) {
        shell_.MakeMenuCoordinator().CloseMenuBar();
      } else {
        shell_.MakeMenuCoordinator().OpenAnchoredMenu(MenuId::SidebarMode, sidebar_mode_rect);
      }
      shell_.surface_.focus = WorkspaceShell::FocusTarget::Sidebar;
      shell_.RequestChromeRedraw();
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

bool ChromeMouseCoordinator::HandleMotion(const SDL_Event& event,
                                          const WorkspaceLayout& layout) {
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

  if (!shell_.overlay_state_.visible) {
    return false;
  }

  const int overlay_ticks = vertical_ticks != 0 ? vertical_ticks : horizontal_ticks;
  if (shell_.overlay_state_.mode == WorkspaceShell::OverlayMode::CommitPicker) {
    shell_.MoveComparePickerSelection(-overlay_ticks);
  } else if (shell_.overlay_state_.mode == WorkspaceShell::OverlayMode::BufferSearch ||
             shell_.overlay_state_.mode == WorkspaceShell::OverlayMode::BufferReplace) {
    shell_.MoveBufferSearchSelection(-overlay_ticks);
  } else if (shell_.overlay_state_.mode == WorkspaceShell::OverlayMode::ProjectSearch) {
    shell_.MoveProjectSearchSelection(-overlay_ticks);
  } else {
    shell_.MoveFileFinderSelection(-overlay_ticks);
  }
  shell_.RequestOverlayRedraw();
  return true;
}

bool ChromeMouseCoordinator::HandleMenuButtonDown(const SDL_Event& event,
                                                  const WorkspaceLayout& layout) {
  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }

  const auto menu_bar_items = shell_.ComputeVisibleMenuBarItems(layout.menu_bar);
  const auto window_buttons =
      shell_.ComputeVisibleWindowControlButtons(layout.menu_bar);
  for (const WorkspaceShell::VisibleWindowControlButton& button : window_buttons) {
    if (!Contains(button.rect, event.button.x, event.button.y)) {
      continue;
    }
    shell_.MakeMenuCoordinator().CloseMenuBar();
    switch (button.id) {
      case WorkspaceShell::WindowControlButtonId::Minimize:
        shell_.pending_window_action_ = WorkspaceShell::WindowAction::Minimize;
        break;
      case WorkspaceShell::WindowControlButtonId::Maximize:
        shell_.pending_window_action_ = WorkspaceShell::WindowAction::ToggleMaximize;
        break;
      case WorkspaceShell::WindowControlButtonId::Close:
        shell_.RequestQuit();
        break;
    }
    shell_.RequestChromeRedraw();
    return true;
  }

  if (shell_.menu_state_.menu_bar_open) {
    for (const WorkspaceShell::VisibleMenuBarItem& item : menu_bar_items) {
      if (!Contains(item.rect, event.button.x, event.button.y)) {
        continue;
      }
      if (item.id == shell_.menu_state_.active_menu_id) {
        shell_.MakeMenuCoordinator().CloseMenuBar();
      } else {
        shell_.MakeMenuCoordinator().OpenMenuBarMenu(item.id);
      }
      shell_.RequestChromeRedraw();
      return true;
    }

    if (const auto submenu_rect = shell_.ActiveSubmenuRect(layout.menu_bar);
        submenu_rect.has_value() &&
        Contains(*submenu_rect, event.button.x, event.button.y)) {
      for (const WorkspaceShell::VisiblePopupMenuItem& item :
           shell_.ComputeVisiblePopupMenuItems(shell_.menu_state_.active_submenu_id,
                                              *submenu_rect)) {
        if (!Contains(item.rect, event.button.x, event.button.y)) {
          continue;
        }
        shell_.menu_state_.active_submenu_item_index =
            item.enabled ? static_cast<int>(item.index) : -1;
        if (!item.separator && item.enabled) {
          shell_.MakeMenuCoordinator().ExecuteMenuItem(shell_.menu_state_.active_submenu_id,
                                                       item.index);
        }
        shell_.RequestChromeRedraw();
        return true;
      }
      shell_.RequestChromeRedraw();
      return true;
    }

    if (const auto popup_rect =
            shell_.ComputePopupMenuRect(layout.menu_bar, shell_.menu_state_.active_menu_id);
        popup_rect.has_value() && Contains(*popup_rect, event.button.x, event.button.y)) {
      for (const WorkspaceShell::VisiblePopupMenuItem& item :
           shell_.ComputeVisiblePopupMenuItems(shell_.menu_state_.active_menu_id,
                                              *popup_rect)) {
        if (!Contains(item.rect, event.button.x, event.button.y)) {
          continue;
        }
        shell_.menu_state_.active_menu_item_index =
            item.enabled ? static_cast<int>(item.index) : -1;
        if (!item.separator && item.enabled) {
          shell_.MakeMenuCoordinator().ExecuteMenuItem(shell_.menu_state_.active_menu_id,
                                                       item.index);
        }
        shell_.RequestChromeRedraw();
        return true;
      }
      shell_.RequestChromeRedraw();
      return true;
    }

    shell_.MakeMenuCoordinator().CloseMenuBar();
    shell_.RequestChromeRedraw();
    return true;
  }

  if (!Contains(layout.menu_bar, event.button.x, event.button.y)) {
    return false;
  }

  for (const WorkspaceShell::VisibleMenuBarItem& item : menu_bar_items) {
    if (Contains(item.rect, event.button.x, event.button.y)) {
      shell_.MakeMenuCoordinator().OpenMenuBarMenu(item.id);
      shell_.RequestChromeRedraw();
      return true;
    }
  }
  if (event.button.clicks >= 2) {
    shell_.pending_window_action_ = WorkspaceShell::WindowAction::ToggleMaximize;
  }
  shell_.RequestChromeRedraw();
  return true;
}

bool ChromeMouseCoordinator::HandleMenuMotion(const SDL_Event& event,
                                              const WorkspaceLayout& layout) {
  if (!shell_.menu_state_.menu_bar_open) {
    return false;
  }

  for (const WorkspaceShell::VisibleMenuBarItem& item :
       shell_.ComputeVisibleMenuBarItems(layout.menu_bar)) {
    if (!Contains(item.rect, event.motion.x, event.motion.y)) {
      continue;
    }
    if (item.id != shell_.menu_state_.active_menu_id) {
      shell_.MakeMenuCoordinator().OpenMenuBarMenu(item.id);
    }
    shell_.RequestChromeRedraw();
    return true;
  }

  if (const auto submenu_rect = shell_.ActiveSubmenuRect(layout.menu_bar);
      submenu_rect.has_value() && Contains(*submenu_rect, event.motion.x, event.motion.y)) {
    shell_.menu_state_.active_submenu_item_index = -1;
    for (const WorkspaceShell::VisiblePopupMenuItem& item :
         shell_.ComputeVisiblePopupMenuItems(shell_.menu_state_.active_submenu_id,
                                            *submenu_rect)) {
      if (Contains(item.rect, event.motion.x, event.motion.y)) {
        shell_.menu_state_.active_submenu_item_index =
            item.enabled ? static_cast<int>(item.index) : -1;
        break;
      }
    }
    shell_.RequestChromeRedraw();
    return true;
  }

  if (const auto popup_rect =
          shell_.ComputePopupMenuRect(layout.menu_bar, shell_.menu_state_.active_menu_id);
      popup_rect.has_value() && Contains(*popup_rect, event.motion.x, event.motion.y)) {
    shell_.menu_state_.active_menu_item_index = -1;
    for (const WorkspaceShell::VisiblePopupMenuItem& item :
         shell_.ComputeVisiblePopupMenuItems(shell_.menu_state_.active_menu_id, *popup_rect)) {
      if (Contains(item.rect, event.motion.x, event.motion.y)) {
        shell_.menu_state_.active_menu_item_index =
            item.enabled ? static_cast<int>(item.index) : -1;
        const MenuSpec* menu = shell_.FindMenuSpec(shell_.menu_state_.active_menu_id);
        if (menu != nullptr && item.enabled) {
          const auto items = shell_.MenuItems(shell_.menu_state_.active_menu_id);
          const MenuItemSpec& spec = items[item.index];
          if (spec.submenu != MenuId::None) {
            shell_.MakeMenuCoordinator().OpenSubmenu(spec.submenu, item.rect);
          } else {
            shell_.MakeMenuCoordinator().CloseSubmenu();
          }
        } else {
          shell_.MakeMenuCoordinator().CloseSubmenu();
        }
        break;
      }
    }
    shell_.RequestChromeRedraw();
    return true;
  }

  shell_.menu_state_.active_menu_item_index = -1;
  shell_.RequestChromeRedraw();
  return true;
}

bool ChromeMouseCoordinator::HandleOverlayButtonDown(const SDL_Event& event,
                                                     const WorkspaceLayout& layout) {
  if (!shell_.overlay_state_.visible || event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }

  const SDL_FRect overlay = shell_.ComputeOverlayRect(layout.editor_area);
  if (!Contains(overlay, event.button.x, event.button.y)) {
    shell_.DismissOverlay();
    shell_.RequestOverlayRedraw();
    return true;
  }

  shell_.ClampOverlayScrollRow(overlay);
  const auto list_layout = shell_.ComputeOverlayListLayout(overlay);
  if (list_layout.scrollbar.has_value() &&
      Contains(list_layout.scrollbar->track, event.button.x, event.button.y)) {
    shell_.interaction_state_.drag_target = WorkspaceShell::DragTarget::OverlayScrollbar;
    shell_.interaction_state_.drag_scrollbar_offset =
        Contains(list_layout.scrollbar->thumb, event.button.x, event.button.y)
            ? static_cast<float>(event.button.y) - list_layout.scrollbar->thumb.y
            : list_layout.scrollbar->thumb.h * 0.5f;
    shell_.overlay_state_.scroll_row = std::clamp(
        static_cast<int>(std::lround(ScrollUnitsForPointer(
            *list_layout.scrollbar, static_cast<float>(event.button.y),
            shell_.interaction_state_.drag_scrollbar_offset))),
        0, list_layout.max_scroll);
    shell_.surface_.focus = WorkspaceShell::FocusTarget::Overlay;
    shell_.RequestOverlayRedraw();
    return true;
  }

  if (const auto item_index =
          ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
      item_index.has_value() && *item_index >= 0 &&
      *item_index < static_cast<int>(shell_.OverlayItemCount())) {
    shell_.SetOverlaySelectedIndex(static_cast<std::size_t>(*item_index));
    shell_.RevealOverlaySelection(overlay);
    if (shell_.overlay_state_.mode == WorkspaceShell::OverlayMode::CommitPicker) {
      shell_.ActivateOverlaySelection();
    }
  }
  shell_.surface_.focus = WorkspaceShell::FocusTarget::Overlay;
  shell_.RequestOverlayRedraw();
  return true;
}

bool ChromeMouseCoordinator::HandleTreeContextMenuButtonDown(const SDL_Event& event) {
  if (!shell_.menu_state_.tree_context_menu.open) {
    return false;
  }

  if (const auto popup_rect = shell_.ComputeTreeContextMenuRect();
      popup_rect.has_value() &&
      Contains(*popup_rect, event.button.x, event.button.y)) {
    for (const WorkspaceShell::VisiblePopupMenuItem& item : shell_.ComputeVisiblePopupMenuItems(
             WorkspaceShell::TreeContextMenuItems(shell_.menu_state_.tree_context_menu.target),
             shell_.menu_state_.tree_context_menu.active_item_index, *popup_rect)) {
      if (!Contains(item.rect, event.button.x, event.button.y)) {
        continue;
      }
      shell_.menu_state_.tree_context_menu.active_item_index =
          item.enabled ? static_cast<int>(item.index) : -1;
      if (event.button.button == SDL_BUTTON_LEFT && !item.separator && item.enabled) {
        shell_.MakeMenuCoordinator().ExecuteTreeContextMenuItem(item.index);
      }
      shell_.RequestChromeRedraw();
      return true;
    }
    shell_.RequestChromeRedraw();
    return true;
  }

  shell_.MakeMenuCoordinator().CloseTreeContextMenu();
  shell_.RequestChromeRedraw();
  return false;
}

bool ChromeMouseCoordinator::HandleTreeContextMenuMotion(const SDL_Event& event) {
  if (!shell_.menu_state_.tree_context_menu.open) {
    return false;
  }

  if (const auto popup_rect = shell_.ComputeTreeContextMenuRect();
      popup_rect.has_value() &&
      Contains(*popup_rect, event.motion.x, event.motion.y)) {
    shell_.menu_state_.tree_context_menu.active_item_index = -1;
    for (const WorkspaceShell::VisiblePopupMenuItem& item : shell_.ComputeVisiblePopupMenuItems(
             WorkspaceShell::TreeContextMenuItems(shell_.menu_state_.tree_context_menu.target),
             shell_.menu_state_.tree_context_menu.active_item_index, *popup_rect)) {
      if (Contains(item.rect, event.motion.x, event.motion.y)) {
        shell_.menu_state_.tree_context_menu.active_item_index =
            item.enabled ? static_cast<int>(item.index) : -1;
        break;
      }
    }
    shell_.RequestChromeRedraw();
    return true;
  }

  shell_.menu_state_.tree_context_menu.active_item_index = -1;
  shell_.RequestChromeRedraw();
  return true;
}

}  // namespace microide::workspace
