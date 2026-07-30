#include "workspace/WorkspaceMenuCoordinator.h"

#include <algorithm>
#include <utility>

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceMenuRegistry.h"
#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

MenuCoordinator::MenuCoordinator(MenuSurfaceState& menu_state, Operations operations)
    : menu_state_(menu_state), operations_(std::move(operations)) {}

int MenuCoordinator::FirstEnabledMenuItemIndex(MenuId id) const {
  const auto items = operations_.menu_items(id);
  if (items.empty()) {
    return -1;
  }

  for (std::size_t i = 0; i < items.size(); ++i) {
    if (operations_.is_menu_item_enabled(items[i])) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int MenuCoordinator::NextEnabledMenuItemIndex(MenuId id, int current_index, int delta) const {
  const auto items = operations_.menu_items(id);
  if (items.empty() || delta == 0) {
    return -1;
  }

  const int item_count = static_cast<int>(items.size());
  int index = current_index < 0 ? (delta > 0 ? -1 : 0) : current_index;
  for (int step = 0; step < item_count; ++step) {
    index = (index + delta + item_count) % item_count;
    if (operations_.is_menu_item_enabled(items[static_cast<std::size_t>(index)])) {
      return index;
    }
  }
  return current_index;
}

void MenuCoordinator::OpenMenuBarMenu(MenuId id) {
  operations_.request_chrome_redraw();
  if (id == MenuId::None) {
    CloseMenuBar();
    return;
  }
  CloseTreeContextMenu();
  menu_state_.menu_bar_open = true;
  menu_state_.active_menu_id = id;
  menu_state_.active_menu_item_index = FirstEnabledMenuItemIndex(id);
  menu_state_.hovered_popup_row_index = -1;
  menu_state_.active_menu_anchor_rect.reset();
  CloseSubmenu();
  operations_.request_chrome_redraw();
}

void MenuCoordinator::OpenAnchoredMenu(MenuId id, const SDL_FRect& anchor_rect) {
  operations_.request_chrome_redraw();
  if (id == MenuId::None) {
    CloseMenuBar();
    return;
  }
  CloseTreeContextMenu();
  menu_state_.menu_bar_open = true;
  menu_state_.active_menu_id = id;
  menu_state_.active_menu_item_index = FirstEnabledMenuItemIndex(id);
  menu_state_.hovered_popup_row_index = -1;
  menu_state_.active_menu_anchor_rect = anchor_rect;
  CloseSubmenu();
  operations_.request_chrome_redraw();
}

void MenuCoordinator::OpenSubmenu(MenuId id, const SDL_FRect& anchor_rect) {
  operations_.request_chrome_redraw();
  menu_state_.active_submenu_id = id;
  menu_state_.active_submenu_item_index = FirstEnabledMenuItemIndex(id);
  menu_state_.hovered_submenu_row_index = -1;
  menu_state_.active_submenu_anchor_rect = anchor_rect;
  operations_.request_chrome_redraw();
}

void MenuCoordinator::CloseSubmenu() {
  operations_.request_chrome_redraw();
  menu_state_.active_submenu_id = MenuId::None;
  menu_state_.active_submenu_item_index = -1;
  menu_state_.hovered_submenu_row_index = -1;
  menu_state_.active_submenu_anchor_rect.reset();
  operations_.request_chrome_redraw();
}

void MenuCoordinator::CloseMenuBar() {
  operations_.request_chrome_redraw();
  menu_state_.menu_bar_open = false;
  menu_state_.active_menu_id = MenuId::None;
  menu_state_.active_menu_item_index = -1;
  menu_state_.hovered_popup_row_index = -1;
  menu_state_.active_menu_anchor_rect.reset();
  CloseSubmenu();
  operations_.request_chrome_redraw();
}

bool MenuCoordinator::ExecuteMenuItem(MenuId menu_id, std::size_t item_index) {
  const auto items = operations_.menu_items(menu_id);
  if (items.empty() || item_index >= items.size()) {
    return false;
  }

  const MenuItemSpec& item = items[item_index];
  if (!operations_.is_menu_item_enabled(item)) {
    return true;
  }
  if (item.submenu != MenuId::None) {
    if (const auto item_rect = operations_.menu_popup_item_rect(menu_id, item_index);
        item_rect.has_value()) {
      OpenSubmenu(item.submenu, *item_rect);
      return true;
    }
    CloseSubmenu();
    return true;
  }

  if (operations_.execute_custom_menu_item != nullptr &&
      operations_.execute_custom_menu_item(menu_id, item_index)) {
    CloseMenuBar();
    return true;
  }

  std::vector<std::string> args;
  args.reserve(item.arg_count);
  for (std::size_t i = 0; i < item.arg_count; ++i) {
    args.emplace_back(item.args[i]);
  }
  CloseMenuBar();
  if (!item.command_name.empty()) {
    return operations_.execute_command_name(item.command_name, args, ActionSource::Menu);
  }
  return operations_.execute_action(item.action, args, ActionSource::Menu);
}

bool MenuCoordinator::SwitchMenuBarMenu(int delta) {
  const auto menus = WorkspaceMenuSpecs();
  if (menus.empty() || menu_state_.active_menu_id == MenuId::None || delta == 0) {
    return false;
  }

  auto current_it = std::find_if(
      menus.begin(), menus.end(),
      [this](const MenuSpec& spec) { return spec.id == menu_state_.active_menu_id; });
  if (current_it == menus.end()) {
    return false;
  }

  const int current_index = static_cast<int>(std::distance(menus.begin(), current_it));
  const int next_index =
      (current_index + delta + static_cast<int>(menus.size())) % static_cast<int>(menus.size());
  OpenMenuBarMenu(menus[static_cast<std::size_t>(next_index)].id);
  return true;
}

bool MenuCoordinator::MoveActiveMenuItem(int delta) {
  if (!menu_state_.menu_bar_open || menu_state_.active_menu_id == MenuId::None) {
    return false;
  }
  menu_state_.active_menu_item_index =
      NextEnabledMenuItemIndex(menu_state_.active_menu_id, menu_state_.active_menu_item_index, delta);
  return menu_state_.active_menu_item_index >= 0;
}

void MenuCoordinator::OpenTreeContextMenu(TreeContextTargetKind target,
                                          const std::filesystem::path& path,
                                          const SDL_FRect& anchor_rect,
                                          std::size_t line) {
  operations_.request_chrome_redraw();
  CloseMenuBar();
  menu_state_.tree_context_menu.open = true;
  menu_state_.tree_context_menu.target = target;
  menu_state_.tree_context_menu.path = path.lexically_normal();
  menu_state_.tree_context_menu.line = line;
  menu_state_.tree_context_menu.anchor_rect = anchor_rect;
  menu_state_.tree_context_menu.active_item_index = FirstEnabledTreeContextMenuItemIndex();
  operations_.request_chrome_redraw();
}

void MenuCoordinator::CloseTreeContextMenu() {
  operations_.request_chrome_redraw();
  menu_state_.tree_context_menu = TreeContextMenuState{};
  operations_.request_chrome_redraw();
}

bool MenuCoordinator::ExecuteTreeContextMenuItem(std::size_t item_index) {
  const auto items = WorkspaceTreeContextMenuItems(menu_state_.tree_context_menu.target);
  if (item_index >= items.size()) {
    return false;
  }

  const MenuItemSpec& item = items[item_index];
  if (!operations_.is_menu_item_enabled(item)) {
    return true;
  }

  std::vector<std::string> args;
  args.reserve(item.arg_count);
  for (std::size_t i = 0; i < item.arg_count; ++i) {
    args.emplace_back(item.args[i]);
  }
  const bool handled = operations_.execute_action(item.action, args, ActionSource::ContextMenu);
  CloseTreeContextMenu();
  return handled;
}

int MenuCoordinator::FirstEnabledTreeContextMenuItemIndex() const {
  const auto items = WorkspaceTreeContextMenuItems(menu_state_.tree_context_menu.target);
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (operations_.is_menu_item_enabled(items[i])) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int MenuCoordinator::NextEnabledTreeContextMenuItemIndex(int current_index, int delta) const {
  const auto items = WorkspaceTreeContextMenuItems(menu_state_.tree_context_menu.target);
  if (items.empty() || delta == 0) {
    return -1;
  }

  const int item_count = static_cast<int>(items.size());
  int index = current_index < 0 ? (delta > 0 ? -1 : 0) : current_index;
  for (int step = 0; step < item_count; ++step) {
    index = (index + delta + item_count) % item_count;
    if (operations_.is_menu_item_enabled(items[static_cast<std::size_t>(index)])) {
      return index;
    }
  }
  return current_index;
}

MenuCoordinator WorkspaceShell::MakeMenuCoordinator() {
  return MenuCoordinator(
      context_.menu_state,
      MenuCoordinator::Operations{
          .request_chrome_redraw = [this]() { RequestChromeRedraw(); },
          .menu_items = [this](MenuId id) { return MenuItems(id); },
          .is_menu_item_enabled =
              [this](const MenuItemSpec& item) { return IsMenuItemEnabled(item); },
          .menu_popup_item_rect =
              [this](MenuId id, std::size_t item_index) -> std::optional<SDL_FRect> {
                const auto layout = CurrentWorkspaceLayout();
                if (!layout.has_value()) {
                  return std::nullopt;
                }
                const auto popup_rect = ComputePopupMenuRect(layout->menu_bar, id);
                if (!popup_rect.has_value()) {
                  return std::nullopt;
                }
                for (const auto& item : ComputeVisiblePopupMenuItems(id, *popup_rect)) {
                  if (item.index == item_index) {
                    return item.rect;
                  }
                }
                return std::nullopt;
              },
          .execute_custom_menu_item =
              [this](MenuId id, std::size_t item_index) {
                return ExecuteCustomMenuItem(id, item_index);
              },
          .execute_action =
              [this](ActionId id, const std::vector<std::string>& args, ActionSource source) {
                return ActionCoordinator(MakeActionContext()).Execute(id, args, source);
              },
          .execute_command_name =
              [this](std::string_view command_name,
                     const std::vector<std::string>& args,
                     ActionSource source) {
                return ExecuteCommandName(command_name, args, source);
              },
      });
}

}  // namespace microide::workspace
