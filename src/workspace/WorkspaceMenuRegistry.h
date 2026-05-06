#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceActionTypes.h"
#include "workspace/WorkspaceLspClient.h"

namespace microide::plugin {
class PluginHost;
}  // namespace microide::plugin

namespace microide::workspace {

enum class MenuId {
  None,
  File,
  Edit,
  View,
  SidebarMode,
  GitOutgoingBase,
  Search,
  EditorContext,
  EditorTabContext,
  Project,
  Terminal,
  TerminalContext,
  TerminalTabContext,
};

enum class TreeContextTargetKind {
  None,
  File,
  Directory,
  Root,
  Background,
};

struct MenuItemSpec {
  ActionId action = ActionId::Colorscheme;
  std::string_view label;
  std::string_view accelerator;
  std::array<std::string_view, 2> args{};
  std::size_t arg_count = 0;
  bool separator = false;
  bool checkable = false;
  MenuId submenu = MenuId::None;
  std::string_view command_name;
};

struct MenuSpec {
  MenuId id = MenuId::None;
  std::string_view label;
  std::span<const MenuItemSpec> items;
};

std::span<const MenuSpec> WorkspaceMenuSpecs();
const MenuSpec* FindWorkspaceMenuSpec(MenuId id);
std::span<const MenuItemSpec> WorkspaceTreeContextMenuItems(TreeContextTargetKind target);
bool IsLspDrivenMenuAction(ActionId id);
bool IsLspMenuActionReady(const LspClient::ReadinessSnapshot& snapshot);
std::string LspDrivenMenuActionLabel(ActionId id,
                                     std::string_view ready_label,
                                     const LspClient::ReadinessSnapshot& snapshot);

// Map a plugin menu string ("file", "edit", "view", "search") to a MenuId.
// Returns MenuId::None for unrecognised values.
MenuId ParseMenuId(std::string_view name);

// Dynamic menu items contributed by plugins for a given menu.
// Returned items use owning strings because plugin data is not static.
struct ContributedMenuItemView {
  std::string id;
  std::string action;      // command name (plugin or built-in)
  std::string label;
  std::string accelerator;
  bool separator_before = false;
  std::string plugin_id;
};

std::vector<ContributedMenuItemView> ContributedMenuItems(
    MenuId menu_id,
    const plugin::PluginHost& plugin_host);

}  // namespace microide::workspace
