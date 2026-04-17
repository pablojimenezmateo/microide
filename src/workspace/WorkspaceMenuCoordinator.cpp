#include "workspace/WorkspaceMenuCoordinator.h"

#include <algorithm>

#include "workspace/WorkspaceActionCoordinator.h"

namespace microide::workspace {

WorkspaceShell::MenuCoordinator::MenuCoordinator(WorkspaceShell& shell) : shell_(shell) {}

int WorkspaceShell::MenuCoordinator::FirstEnabledMenuItemIndex(MenuId id) const {
  const auto items = shell_.MenuItems(id);
  if (items.empty()) {
    return -1;
  }

  for (std::size_t i = 0; i < items.size(); ++i) {
    if (shell_.IsMenuItemEnabled(items[i])) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int WorkspaceShell::MenuCoordinator::NextEnabledMenuItemIndex(MenuId id,
                                                              int current_index,
                                                              int delta) const {
  const auto items = shell_.MenuItems(id);
  if (items.empty() || delta == 0) {
    return -1;
  }

  const int item_count = static_cast<int>(items.size());
  int index = current_index < 0 ? (delta > 0 ? -1 : 0) : current_index;
  for (int step = 0; step < item_count; ++step) {
    index = (index + delta + item_count) % item_count;
    if (shell_.IsMenuItemEnabled(items[static_cast<std::size_t>(index)])) {
      return index;
    }
  }
  return current_index;
}

void WorkspaceShell::MenuCoordinator::OpenMenuBarMenu(MenuId id) {
  shell_.RequestChromeRedraw();
  if (id == MenuId::None) {
    CloseMenuBar();
    return;
  }
  CloseTreeContextMenu();
  shell_.surface_.menu_bar_open = true;
  shell_.surface_.active_menu_id = id;
  shell_.surface_.active_menu_item_index = FirstEnabledMenuItemIndex(id);
  shell_.surface_.active_menu_anchor_rect.reset();
  CloseSubmenu();
  shell_.RequestChromeRedraw();
}

void WorkspaceShell::MenuCoordinator::OpenAnchoredMenu(MenuId id, const SDL_FRect& anchor_rect) {
  shell_.RequestChromeRedraw();
  if (id == MenuId::None) {
    CloseMenuBar();
    return;
  }
  CloseTreeContextMenu();
  shell_.surface_.menu_bar_open = true;
  shell_.surface_.active_menu_id = id;
  shell_.surface_.active_menu_item_index = FirstEnabledMenuItemIndex(id);
  shell_.surface_.active_menu_anchor_rect = anchor_rect;
  CloseSubmenu();
  shell_.RequestChromeRedraw();
}

void WorkspaceShell::MenuCoordinator::OpenSubmenu(MenuId id, const SDL_FRect& anchor_rect) {
  shell_.RequestChromeRedraw();
  shell_.surface_.active_submenu_id = id;
  shell_.surface_.active_submenu_item_index = FirstEnabledMenuItemIndex(id);
  shell_.surface_.active_submenu_anchor_rect = anchor_rect;
  shell_.RequestChromeRedraw();
}

void WorkspaceShell::MenuCoordinator::CloseSubmenu() {
  shell_.RequestChromeRedraw();
  shell_.surface_.active_submenu_id = MenuId::None;
  shell_.surface_.active_submenu_item_index = -1;
  shell_.surface_.active_submenu_anchor_rect.reset();
  shell_.RequestChromeRedraw();
}

void WorkspaceShell::MenuCoordinator::CloseMenuBar() {
  shell_.RequestChromeRedraw();
  shell_.surface_.menu_bar_open = false;
  shell_.surface_.active_menu_id = MenuId::None;
  shell_.surface_.active_menu_item_index = -1;
  shell_.surface_.active_menu_anchor_rect.reset();
  CloseSubmenu();
  shell_.RequestChromeRedraw();
}

bool WorkspaceShell::MenuCoordinator::ExecuteMenuItem(MenuId menu_id, std::size_t item_index) {
  const auto items = shell_.MenuItems(menu_id);
  if (items.empty() || item_index >= items.size()) {
    return false;
  }

  const MenuItemSpec& item = items[item_index];
  if (!shell_.IsMenuItemEnabled(item)) {
    return true;
  }
  if (item.submenu != MenuId::None) {
    if (const auto layout = shell_.CurrentWorkspaceLayout(); layout.has_value()) {
      if (const auto popup_rect = shell_.ComputePopupMenuRect(layout->menu_bar, menu_id);
          popup_rect.has_value()) {
        for (const VisiblePopupMenuItem& visible_item :
             shell_.ComputeVisiblePopupMenuItems(menu_id, *popup_rect)) {
          if (visible_item.index != item_index) {
            continue;
          }
          OpenSubmenu(item.submenu, visible_item.rect);
          return true;
        }
      }
    }
    CloseSubmenu();
    return true;
  }

  std::vector<std::string> args;
  args.reserve(item.arg_count);
  for (std::size_t i = 0; i < item.arg_count; ++i) {
    args.emplace_back(item.args[i]);
  }
  CloseMenuBar();
  return ActionCoordinator(shell_).Execute(item.action, args, ActionSource::Menu);
}

bool WorkspaceShell::MenuCoordinator::SwitchMenuBarMenu(int delta) {
  const auto menus = WorkspaceShell::MenuSpecs();
  if (menus.empty() || shell_.surface_.active_menu_id == MenuId::None || delta == 0) {
    return false;
  }

  auto current_it = std::find_if(
      menus.begin(), menus.end(),
      [this](const MenuSpec& spec) { return spec.id == shell_.surface_.active_menu_id; });
  if (current_it == menus.end()) {
    return false;
  }

  const int current_index = static_cast<int>(std::distance(menus.begin(), current_it));
  const int next_index =
      (current_index + delta + static_cast<int>(menus.size())) % static_cast<int>(menus.size());
  OpenMenuBarMenu(menus[static_cast<std::size_t>(next_index)].id);
  return true;
}

bool WorkspaceShell::MenuCoordinator::MoveActiveMenuItem(int delta) {
  if (!shell_.surface_.menu_bar_open || shell_.surface_.active_menu_id == MenuId::None) {
    return false;
  }
  shell_.surface_.active_menu_item_index = NextEnabledMenuItemIndex(
      shell_.surface_.active_menu_id, shell_.surface_.active_menu_item_index, delta);
  return shell_.surface_.active_menu_item_index >= 0;
}

void WorkspaceShell::MenuCoordinator::OpenTreeContextMenu(TreeContextTargetKind target,
                                                          const std::filesystem::path& path,
                                                          const SDL_FRect& anchor_rect) {
  shell_.RequestChromeRedraw();
  CloseMenuBar();
  shell_.surface_.tree_context_menu.open = true;
  shell_.surface_.tree_context_menu.target = target;
  shell_.surface_.tree_context_menu.path = path.lexically_normal();
  shell_.surface_.tree_context_menu.anchor_rect = anchor_rect;
  shell_.surface_.tree_context_menu.active_item_index = FirstEnabledTreeContextMenuItemIndex();
  shell_.RequestChromeRedraw();
}

void WorkspaceShell::MenuCoordinator::CloseTreeContextMenu() {
  shell_.RequestChromeRedraw();
  shell_.surface_.tree_context_menu = TreeContextMenuState{};
  shell_.RequestChromeRedraw();
}

bool WorkspaceShell::MenuCoordinator::ExecuteTreeContextMenuItem(std::size_t item_index) {
  const auto items = WorkspaceShell::TreeContextMenuItems(shell_.surface_.tree_context_menu.target);
  if (item_index >= items.size()) {
    return false;
  }

  const MenuItemSpec& item = items[item_index];
  if (!shell_.IsMenuItemEnabled(item)) {
    return true;
  }

  std::vector<std::string> args;
  args.reserve(item.arg_count);
  for (std::size_t i = 0; i < item.arg_count; ++i) {
    args.emplace_back(item.args[i]);
  }
  const bool handled =
      ActionCoordinator(shell_).Execute(item.action, args, ActionSource::ContextMenu);
  CloseTreeContextMenu();
  return handled;
}

int WorkspaceShell::MenuCoordinator::FirstEnabledTreeContextMenuItemIndex() const {
  const auto items = WorkspaceShell::TreeContextMenuItems(shell_.surface_.tree_context_menu.target);
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (shell_.IsMenuItemEnabled(items[i])) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int WorkspaceShell::MenuCoordinator::NextEnabledTreeContextMenuItemIndex(int current_index,
                                                                         int delta) const {
  const auto items = WorkspaceShell::TreeContextMenuItems(shell_.surface_.tree_context_menu.target);
  if (items.empty() || delta == 0) {
    return -1;
  }

  const int item_count = static_cast<int>(items.size());
  int index = current_index < 0 ? (delta > 0 ? -1 : 0) : current_index;
  for (int step = 0; step < item_count; ++step) {
    index = (index + delta + item_count) % item_count;
    if (shell_.IsMenuItemEnabled(items[static_cast<std::size_t>(index)])) {
      return index;
    }
  }
  return current_index;
}

}  // namespace microide::workspace
