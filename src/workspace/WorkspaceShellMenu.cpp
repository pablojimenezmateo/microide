#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

std::span<const WorkspaceShell::MenuItemSpec> WorkspaceShell::TreeContextMenuItems(
    TreeContextTargetKind target) {
  const auto item = [](ActionId action, std::string_view label = {}) {
    return MenuItemSpec{action, label, {}, {}, 0, false, false};
  };
  const auto separator = [] {
    return MenuItemSpec{ActionId::Colorscheme, {}, {}, {}, 0, true, false};
  };

  static const auto kFileItems = std::to_array<MenuItemSpec>({
      item(ActionId::OpenSelectedTreeItem),
      item(ActionId::OpenSelectedTreeItemInNewTab),
      separator(),
      item(ActionId::CompareHead),
      item(ActionId::Compare),
      separator(),
      item(ActionId::RenamePath),
      item(ActionId::DeletePath),
      separator(),
      item(ActionId::CopyRelativePath),
      item(ActionId::CopyAbsolutePath),
  });
  static const auto kDirectoryItems = std::to_array<MenuItemSpec>({
      item(ActionId::CreateFile),
      item(ActionId::CreateDirectory),
      separator(),
      item(ActionId::RenamePath),
      item(ActionId::DeletePath),
      separator(),
      item(ActionId::TreeRefresh, "Refresh"),
      separator(),
      item(ActionId::CopyRelativePath),
      item(ActionId::CopyAbsolutePath),
  });
  static const auto kRootItems = std::to_array<MenuItemSpec>({
      item(ActionId::CreateFile),
      item(ActionId::CreateDirectory),
      separator(),
      item(ActionId::TreeRefresh, "Refresh"),
      item(ActionId::ProjectClose),
      separator(),
      item(ActionId::CopyAbsolutePath),
  });
  static const auto kBackgroundItems = std::to_array<MenuItemSpec>({
      item(ActionId::CreateFile),
      item(ActionId::CreateDirectory),
      separator(),
      item(ActionId::TreeRefresh, "Refresh"),
  });

  switch (target) {
    case TreeContextTargetKind::File:
      return kFileItems;
    case TreeContextTargetKind::Directory:
      return kDirectoryItems;
    case TreeContextTargetKind::Root:
      return kRootItems;
    case TreeContextTargetKind::Background:
      return kBackgroundItems;
    case TreeContextTargetKind::None:
    default:
      return {};
  }
}

std::vector<WorkspaceShell::VisibleMenuBarItem> WorkspaceShell::ComputeVisibleMenuBarItems(
    const SDL_FRect& menu_bar) const {
  std::vector<VisibleMenuBarItem> items;
  float x = menu_bar.x + 8.0f;
  const float y = menu_bar.y + 3.0f;
  const float height = std::max(18.0f, menu_bar.h - 6.0f);
  const auto window_buttons = ComputeVisibleWindowControlButtons(menu_bar);
  const float max_x = window_buttons.empty()
                          ? menu_bar.x + menu_bar.w - 8.0f
                          : window_buttons.front().rect.x - 8.0f;
  for (const MenuSpec& spec : MenuSpecs()) {
    if (spec.id == MenuId::SidebarMode || spec.id == MenuId::EditorTabContext ||
        spec.id == MenuId::TerminalContext || spec.id == MenuId::TerminalTabContext) {
      continue;
    }
    const float width =
        std::clamp(text_renderer_.MeasureWidth(spec.label) + 28.0f, 56.0f, 116.0f);
    if (x + width > max_x) {
      break;
    }
    items.push_back(VisibleMenuBarItem{
        .id = spec.id,
        .rect = MakeRect(x, y, width, height),
        .active = surface_.menu_bar_open && spec.id == surface_.active_menu_id,
    });
    x += width + 4.0f;
  }
  return items;
}

std::vector<WorkspaceShell::VisibleWindowControlButton>
WorkspaceShell::ComputeVisibleWindowControlButtons(const SDL_FRect& menu_bar) const {
  std::vector<VisibleWindowControlButton> buttons;
  if (!CurrentWindowChromeState().custom_enabled) {
    return buttons;
  }

  const float button_size = std::max(18.0f, menu_bar.h - 6.0f);
  const float total_width =
      button_size * 3.0f + kWorkspaceWindowControlButtonGap * 2.0f;
  const float start_x =
      menu_bar.x + std::max(0.0f, menu_bar.w - total_width -
                                      kWorkspaceWindowControlButtonRightInset);
  const float y = menu_bar.y + (menu_bar.h - button_size) * 0.5f;
  static constexpr auto kButtonIds = std::to_array<WindowControlButtonId>({
      WindowControlButtonId::Minimize,
      WindowControlButtonId::Maximize,
      WindowControlButtonId::Close,
  });

  float x = start_x;
  for (WindowControlButtonId id : kButtonIds) {
    const SDL_FRect rect = MakeRect(x, y, button_size, button_size);
    buttons.push_back(VisibleWindowControlButton{
        .id = id,
        .rect = rect,
        .hovered =
            last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_),
    });
    x += button_size + kWorkspaceWindowControlButtonGap;
  }
  return buttons;
}

std::optional<SDL_FRect> WorkspaceShell::ComputePopupMenuRect(
    const SDL_FRect& anchor_rect,
    std::span<const MenuItemSpec> items,
    const SDL_FRect& bounds) const {
  if (items.empty()) {
    return std::nullopt;
  }

  float width = 172.0f;
  float height = 12.0f;
  for (const MenuItemSpec& item : items) {
    if (item.separator) {
      height += kWorkspaceMenuPopupSeparatorHeight;
      continue;
    }
    width = std::max(width, text_renderer_.MeasureWidth(MenuItemLabel(item)) +
                                text_renderer_.MeasureWidth(MenuItemAccelerator(item)) + 68.0f);
    height += kWorkspaceMenuPopupItemHeight;
  }

  const float max_width = std::max(172.0f, bounds.w - 8.0f);
  width = std::clamp(width, 172.0f, max_width);
  float x = std::clamp(anchor_rect.x, bounds.x + 4.0f,
                       bounds.x + std::max(4.0f, bounds.w - width - 4.0f));
  float y = anchor_rect.y + std::max(0.0f, anchor_rect.h) - 1.0f;
  if (y + height > bounds.y + bounds.h - 4.0f) {
    y = anchor_rect.y - height + 1.0f;
  }
  y = std::clamp(y, bounds.y + 4.0f, bounds.y + std::max(4.0f, bounds.h - height - 4.0f));
  return MakeRect(x, y, width, height);
}

std::optional<SDL_FRect> WorkspaceShell::ComputePopupMenuRect(const SDL_FRect& menu_bar,
                                                              MenuId id) const {
  const MenuSpec* menu = FindMenuSpec(id);
  if (menu == nullptr) {
    return std::nullopt;
  }
  const auto items = MenuItems(id);

  const SDL_FRect bounds = CurrentWindowRect().value_or(
      MakeRect(0.0f, 0.0f, menu_bar.w, std::max(menu_bar.y + menu_bar.h + 320.0f, menu_bar.h)));
  if (surface_.active_menu_anchor_rect.has_value() && id == surface_.active_menu_id) {
    return ComputePopupMenuRect(*surface_.active_menu_anchor_rect, items, bounds);
  }

  const auto menu_bar_items = ComputeVisibleMenuBarItems(menu_bar);
  const auto bar_it =
      std::find_if(menu_bar_items.begin(), menu_bar_items.end(),
                   [id](const VisibleMenuBarItem& item) { return item.id == id; });
  if (bar_it == menu_bar_items.end()) {
    return std::nullopt;
  }
  return ComputePopupMenuRect(bar_it->rect, items, bounds);
}

std::vector<WorkspaceShell::VisiblePopupMenuItem> WorkspaceShell::ComputeVisiblePopupMenuItems(
    std::span<const MenuItemSpec> items,
    int active_item_index,
    const SDL_FRect& popup_rect) const {
  std::vector<VisiblePopupMenuItem> visible_items;
  float y = popup_rect.y + 6.0f;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const MenuItemSpec& item = items[i];
    const float height = item.separator ? kWorkspaceMenuPopupSeparatorHeight
                                        : kWorkspaceMenuPopupItemHeight;
    const SDL_FRect rect =
        MakeRect(popup_rect.x + 6.0f, y, std::max(0.0f, popup_rect.w - 12.0f), height);
    visible_items.push_back(VisiblePopupMenuItem{
        .index = i,
        .rect = rect,
        .enabled = IsMenuItemEnabled(item),
        .checked = IsMenuItemChecked(item),
        .hovered = static_cast<int>(i) == active_item_index,
        .separator = item.separator,
    });
    y += height;
  }
  return visible_items;
}

std::vector<WorkspaceShell::VisiblePopupMenuItem> WorkspaceShell::ComputeVisiblePopupMenuItems(
    MenuId id,
    const SDL_FRect& popup_rect) const {
  const auto items = MenuItems(id);
  return items.empty() ? std::vector<VisiblePopupMenuItem>{}
                       : ComputeVisiblePopupMenuItems(items, surface_.active_menu_item_index,
                                                      popup_rect);
}

std::string WorkspaceShell::MenuItemLabel(const MenuItemSpec& item) const {
  if (!item.label.empty()) {
    return std::string(item.label);
  }
  if (const ActionSpec* action = FindActionSpec(item.action); action != nullptr &&
                                                        !action->label.empty()) {
    return std::string(action->label);
  }
  if (const ActionSpec* action = FindActionSpec(item.action); action != nullptr &&
                                                        !action->command_name.empty()) {
    return std::string(action->command_name);
  }
  return {};
}

std::string WorkspaceShell::MenuItemAccelerator(const MenuItemSpec& item) const {
  if (item.submenu != MenuId::None) {
    return ">";
  }
  if (!item.accelerator.empty()) {
    return std::string(item.accelerator);
  }
  if (const ActionSpec* action = FindActionSpec(item.action); action != nullptr &&
                                                        !action->accelerator.empty()) {
    return std::string(action->accelerator);
  }
  return {};
}

bool WorkspaceShell::IsMenuItemEnabled(const MenuItemSpec& item) const {
  if (item.separator) {
    return false;
  }

  if (item.action == ActionId::Files) {
    return !project_root_.empty();
  }
  if (item.action == ActionId::Focus && item.arg_count > 0) {
    if (item.args[0] == "sidebar") {
      return surface_.sidebar_visible;
    }
    if (item.args[0] == "panel") {
      return surface_.command_mode || ActiveTerminalTab() != nullptr;
    }
    return true;
  }
  if ((item.action == ActionId::SidebarShow || item.action == ActionId::SidebarToggle) &&
      item.arg_count > 0) {
    if (FindBuiltinSidebarTool(item.args[0]) != nullptr) {
      return !project_root_.empty();
    }
    return plugin_host_.FindSidebarProvider(item.args[0]) != nullptr;
  }

  return IsActionEnabled(item.action);
}

bool WorkspaceShell::IsMenuItemChecked(const MenuItemSpec& item) const {
  if (!item.checkable) {
    return false;
  }

  if (item.action == ActionId::SidebarToggle) {
    return surface_.sidebar_visible;
  }
  if (item.action == ActionId::SidebarShow && item.arg_count > 0) {
    if (const SidebarToolSpec* tool = FindBuiltinSidebarTool(item.args[0]); tool != nullptr) {
      if (tool->mode == SidebarMode::Tree) {
        return surface_.sidebar_visible && surface_.sidebar_mode == SidebarMode::Tree;
      }
      if (tool->mode == SidebarMode::Search) {
        return surface_.sidebar_visible && surface_.sidebar_mode == SidebarMode::Search &&
               !surface_.sidebar_temporary;
      }
      if (tool->mode == SidebarMode::Problems) {
        return surface_.sidebar_visible && surface_.sidebar_mode == SidebarMode::Problems;
      }
      if (tool->mode == SidebarMode::Git) {
        return surface_.sidebar_visible && surface_.sidebar_mode == SidebarMode::Git;
      }
    }
    if (plugin_host_.FindSidebarProvider(item.args[0]) != nullptr) {
      return surface_.sidebar_visible && surface_.sidebar_mode == SidebarMode::Plugin &&
             surface_.sidebar_plugin_id == item.args[0];
    }
  }
  return false;
}

int WorkspaceShell::FirstEnabledMenuItemIndex(MenuId id) const {
  const auto items = MenuItems(id);
  if (items.empty()) {
    return -1;
  }

  for (std::size_t i = 0; i < items.size(); ++i) {
    if (IsMenuItemEnabled(items[i])) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int WorkspaceShell::NextEnabledMenuItemIndex(MenuId id, int current_index, int delta) const {
  const auto items = MenuItems(id);
  if (items.empty() || delta == 0) {
    return -1;
  }

  const int item_count = static_cast<int>(items.size());
  int index = current_index < 0 ? (delta > 0 ? -1 : 0) : current_index;
  for (int step = 0; step < item_count; ++step) {
    index = (index + delta + item_count) % item_count;
    if (IsMenuItemEnabled(items[static_cast<std::size_t>(index)])) {
      return index;
    }
  }
  return current_index;
}

void WorkspaceShell::OpenMenuBarMenu(MenuId id) {
  RequestChromeRedraw();
  if (id == MenuId::None) {
    CloseMenuBar();
    return;
  }
  CloseTreeContextMenu();
  surface_.menu_bar_open = true;
  surface_.active_menu_id = id;
  surface_.active_menu_item_index = FirstEnabledMenuItemIndex(id);
  surface_.active_menu_anchor_rect.reset();
  CloseSubmenu();
  RequestChromeRedraw();
}

void WorkspaceShell::OpenAnchoredMenu(MenuId id, const SDL_FRect& anchor_rect) {
  RequestChromeRedraw();
  if (id == MenuId::None) {
    CloseMenuBar();
    return;
  }
  CloseTreeContextMenu();
  surface_.menu_bar_open = true;
  surface_.active_menu_id = id;
  surface_.active_menu_item_index = FirstEnabledMenuItemIndex(id);
  surface_.active_menu_anchor_rect = anchor_rect;
  CloseSubmenu();
  RequestChromeRedraw();
}

void WorkspaceShell::OpenSubmenu(MenuId id, const SDL_FRect& anchor_rect) {
  RequestChromeRedraw();
  surface_.active_submenu_id = id;
  surface_.active_submenu_item_index = FirstEnabledMenuItemIndex(id);
  surface_.active_submenu_anchor_rect = anchor_rect;
  RequestChromeRedraw();
}

void WorkspaceShell::CloseSubmenu() {
  RequestChromeRedraw();
  surface_.active_submenu_id = MenuId::None;
  surface_.active_submenu_item_index = -1;
  surface_.active_submenu_anchor_rect.reset();
  RequestChromeRedraw();
}

void WorkspaceShell::CloseMenuBar() {
  RequestChromeRedraw();
  surface_.menu_bar_open = false;
  surface_.active_menu_id = MenuId::None;
  surface_.active_menu_item_index = -1;
  surface_.active_menu_anchor_rect.reset();
  CloseSubmenu();
  RequestChromeRedraw();
}

std::optional<SDL_FRect> WorkspaceShell::ActiveSubmenuRect(const SDL_FRect& menu_bar) const {
  if (surface_.active_submenu_id == MenuId::None || !surface_.active_submenu_anchor_rect.has_value()) {
    return std::nullopt;
  }
  const MenuSpec* submenu = FindMenuSpec(surface_.active_submenu_id);
  if (submenu == nullptr) {
    return std::nullopt;
  }
  const auto submenu_items = MenuItems(surface_.active_submenu_id);
  const SDL_FRect bounds = CurrentWindowRect().value_or(
      MakeRect(0.0f, 0.0f, menu_bar.w, std::max(menu_bar.y + menu_bar.h + 320.0f, menu_bar.h)));
  SDL_FRect anchor = *surface_.active_submenu_anchor_rect;
  anchor.x += anchor.w - 1.0f;
  return ComputePopupMenuRect(anchor, submenu_items, bounds);
}

bool WorkspaceShell::ExecuteMenuItem(MenuId menu_id, std::size_t item_index) {
  const auto items = MenuItems(menu_id);
  if (items.empty() || item_index >= items.size()) {
    return false;
  }

  const MenuItemSpec& item = items[item_index];
  if (!IsMenuItemEnabled(item)) {
    return true;
  }
  if (item.submenu != MenuId::None) {
    if (const auto layout = CurrentWorkspaceLayout(); layout.has_value()) {
      if (const auto popup_rect = ComputePopupMenuRect(layout->menu_bar, menu_id);
          popup_rect.has_value()) {
        for (const VisiblePopupMenuItem& visible_item :
             ComputeVisiblePopupMenuItems(menu_id, *popup_rect)) {
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
  return ExecuteAction(item.action, args, ActionSource::Menu);
}

bool WorkspaceShell::SwitchMenuBarMenu(int delta) {
  const auto menus = MenuSpecs();
  if (menus.empty() || surface_.active_menu_id == MenuId::None || delta == 0) {
    return false;
  }

  auto current_it = std::find_if(menus.begin(), menus.end(),
                                 [this](const MenuSpec& spec) { return spec.id == surface_.active_menu_id; });
  if (current_it == menus.end()) {
    return false;
  }

  const int current_index = static_cast<int>(std::distance(menus.begin(), current_it));
  const int next_index =
      (current_index + delta + static_cast<int>(menus.size())) % static_cast<int>(menus.size());
  OpenMenuBarMenu(menus[static_cast<std::size_t>(next_index)].id);
  return true;
}

bool WorkspaceShell::MoveActiveMenuItem(int delta) {
  if (!surface_.menu_bar_open || surface_.active_menu_id == MenuId::None) {
    return false;
  }
  surface_.active_menu_item_index = NextEnabledMenuItemIndex(surface_.active_menu_id, surface_.active_menu_item_index, delta);
  return surface_.active_menu_item_index >= 0;
}

std::filesystem::path WorkspaceShell::SelectedTreePath() const {
  const project::TreeEntry* entry = SelectedTreeEntry();
  return entry == nullptr ? std::filesystem::path{} : entry->path.lexically_normal();
}

std::filesystem::path WorkspaceShell::ResolveTreeActionPath(ActionSource source) const {
  if (source == ActionSource::ContextMenu && surface_.tree_context_menu.open &&
      !surface_.tree_context_menu.path.empty()) {
    return surface_.tree_context_menu.path.lexically_normal();
  }
  return SelectedTreePath();
}

std::optional<SDL_FRect> WorkspaceShell::ComputeTreeContextMenuRect() const {
  const auto window_rect = CurrentWindowRect();
  if (!surface_.tree_context_menu.open || !window_rect.has_value()) {
    return std::nullopt;
  }
  return ComputePopupMenuRect(surface_.tree_context_menu.anchor_rect,
                              TreeContextMenuItems(surface_.tree_context_menu.target), *window_rect);
}

void WorkspaceShell::OpenTreeContextMenu(TreeContextTargetKind target,
                                         const std::filesystem::path& path,
                                         const SDL_FRect& anchor_rect) {
  RequestChromeRedraw();
  CloseMenuBar();
  surface_.tree_context_menu.open = true;
  surface_.tree_context_menu.target = target;
  surface_.tree_context_menu.path = path.lexically_normal();
  surface_.tree_context_menu.anchor_rect = anchor_rect;
  surface_.tree_context_menu.active_item_index = FirstEnabledTreeContextMenuItemIndex();
  RequestChromeRedraw();
}

void WorkspaceShell::CloseTreeContextMenu() {
  RequestChromeRedraw();
  surface_.tree_context_menu = TreeContextMenuState{};
  RequestChromeRedraw();
}

bool WorkspaceShell::ExecuteTreeContextMenuItem(std::size_t item_index) {
  const auto items = TreeContextMenuItems(surface_.tree_context_menu.target);
  if (item_index >= items.size()) {
    return false;
  }

  const MenuItemSpec& item = items[item_index];
  if (!IsMenuItemEnabled(item)) {
    return true;
  }

  std::vector<std::string> args;
  args.reserve(item.arg_count);
  for (std::size_t i = 0; i < item.arg_count; ++i) {
    args.emplace_back(item.args[i]);
  }
  const bool handled = ExecuteAction(item.action, args, ActionSource::ContextMenu);
  CloseTreeContextMenu();
  return handled;
}

int WorkspaceShell::FirstEnabledTreeContextMenuItemIndex() const {
  const auto items = TreeContextMenuItems(surface_.tree_context_menu.target);
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (IsMenuItemEnabled(items[i])) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int WorkspaceShell::NextEnabledTreeContextMenuItemIndex(int current_index, int delta) const {
  const auto items = TreeContextMenuItems(surface_.tree_context_menu.target);
  if (items.empty() || delta == 0) {
    return -1;
  }

  const int item_count = static_cast<int>(items.size());
  int index = current_index < 0 ? (delta > 0 ? -1 : 0) : current_index;
  for (int step = 0; step < item_count; ++step) {
    index = (index + delta + item_count) % item_count;
    if (IsMenuItemEnabled(items[static_cast<std::size_t>(index)])) {
      return index;
    }
  }
  return current_index;
}

}  // namespace microide::workspace
