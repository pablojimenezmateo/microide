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
  Go,
  Git,
  SidebarMode,
  GitOutgoingBase,
  EditorContext,
  EditorTabContext,
  Project,
  Terminal,
  Debug,
  Help,
  TerminalContext,
  TerminalTabContext,
  ProjectTabContext,
};

enum class TreeContextTargetKind {
  None,
  File,
  Directory,
  Root,
  Background,
  // Debugger (Phase 6): right-click on the breakpoint gutter for a source line.
  BreakpointLine,
  // Git sidebar: right-click on a changed-file entry row.
  GitEntry,
  // Any sidebar row that points at a file location — a search hit, a problem, a
  // discovered test. All three carry a path, so they share one menu of
  // path-scoped items instead of each growing its own.
  ResultRow,
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
// Feature-setting id ("lsp.*.enabled") gating a menu-present LSP action, or empty
// for non-LSP menu actions. Drives hiding the entry when its feature is disabled.
std::string_view LspMenuActionFeatureId(ActionId id);
bool IsLspMenuActionReady(const LspClient::ReadinessSnapshot& snapshot);
std::string LspDrivenMenuActionLabel(ActionId id,
                                     std::string_view ready_label,
                                     const LspClient::ReadinessSnapshot& snapshot);

// Map a plugin menu string ("file", "edit", "view", "go", "terminal") to a MenuId.
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
