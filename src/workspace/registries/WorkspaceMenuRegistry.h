#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "util/InlineVector.h"
#include "workspace/registries/MenuId.h"
#include "workspace/actions/WorkspaceActionTypes.h"
#include "workspace/lsp/WorkspaceLspClient.h"

namespace microide::plugin {
class PluginHost;
}

namespace microide::workspace {

enum class TreeContextTargetKind {
  None,
  File,
  Directory,
  Root,
  Background,
  BreakpointLine,
  GitEntry,
  ResultRow,
  DebugValueRow,
  GitReviewScope,
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

inline constexpr std::size_t kMaxMenuBarItems = 24;

inline constexpr std::size_t kWindowControlButtonCount = 3;

struct VisibleMenuBarItem {
  MenuId id = MenuId::None;
  SDL_FRect rect{};
  bool active = false;
};

using VisibleMenuBarItems = util::InlineVector<VisibleMenuBarItem, kMaxMenuBarItems>;
using MenuBarOverflowIds = util::InlineVector<MenuId, kMaxMenuBarItems>;

std::span<const MenuSpec> WorkspaceMenuSpecs();
const MenuSpec* FindWorkspaceMenuSpec(MenuId id);
std::span<const MenuItemSpec> WorkspaceTreeContextMenuItems(TreeContextTargetKind target);
bool IsLspDrivenMenuAction(ActionId id);
std::string_view LspMenuActionFeatureId(ActionId id);
bool IsLspMenuActionReady(const LspClient::ReadinessSnapshot& snapshot);
std::string_view LspDrivenMenuActionLabel(ActionId id,
                                          std::string_view ready_label,
                                          const LspClient::ReadinessSnapshot& snapshot,
                                          std::string& scratch);

MenuId ParseMenuId(std::string_view name);

struct ContributedMenuItemView {
  std::string id;
  std::string action;
  std::string label;
  std::string accelerator;
  bool separator_before = false;
  std::string plugin_id;
};

std::vector<ContributedMenuItemView> ContributedMenuItems(
    MenuId menu_id,
    const plugin::PluginHost& plugin_host);

}
