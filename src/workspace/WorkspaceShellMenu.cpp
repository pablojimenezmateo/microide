#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "workspace/WorkspaceSidebarRegistry.h"

namespace microide::workspace {

std::span<const WorkspaceShell::MenuItemSpec> WorkspaceShell::TreeContextMenuItems(
    TreeContextTargetKind target) {
  return WorkspaceTreeContextMenuItems(target);
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
  if (const ActionSpec* action = FindActionSpec(item.action);
      action != nullptr && !action->label.empty()) {
    return std::string(action->label);
  }
  if (const ActionSpec* action = FindActionSpec(item.action);
      action != nullptr && !action->command_name.empty()) {
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
  if (const ActionSpec* action = FindActionSpec(item.action);
      action != nullptr && !action->accelerator.empty()) {
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

}  // namespace microide::workspace
