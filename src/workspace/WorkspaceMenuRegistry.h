#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "workspace/WorkspaceActionTypes.h"

namespace microide::workspace {

enum class MenuId {
  None,
  File,
  Edit,
  View,
  SidebarMode,
  Search,
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
};

struct MenuSpec {
  MenuId id = MenuId::None;
  std::string_view label;
  std::span<const MenuItemSpec> items;
};

std::span<const MenuSpec> WorkspaceMenuSpecs();
const MenuSpec* FindWorkspaceMenuSpec(MenuId id);
std::span<const MenuItemSpec> WorkspaceTreeContextMenuItems(TreeContextTargetKind target);

}  // namespace microide::workspace
