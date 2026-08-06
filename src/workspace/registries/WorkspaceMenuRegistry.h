#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "util/InlineVector.h"
#include "workspace/actions/WorkspaceActionTypes.h"
#include "workspace/lsp/WorkspaceLspClient.h"

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
  // Debug pane Variables / Watch row. Acts on the pane's selected row rather than
  // a path, so it carries no path of its own.
  DebugValueRow,
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

// Hard cap on menu-bar entries, and on `WorkspaceMenuSpecs()` itself.
//
// The menu bar's length is a property of the static spec table, not of any data
// the user can produce, which is exactly the precondition `util::InlineVector`
// exists for: laying it out is heap-free. Before that, a single pointer motion
// over the bar rebuilt three `std::vector`s about ten times — 32 of the 50
// allocations per motion event were this one function (TD-2026-08-06-149).
//
// ONE constant so raising it cannot leave a container behind, and it bounds the
// whole table rather than the menu-bar-visible subset: `IsMenuBarTopLevelMenu`
// lives in another translation unit, so the table is what can be checked at
// compile time, and it is the larger number. Asserted against the table in
// WorkspaceMenuRegistry.cpp.
inline constexpr std::size_t kMaxMenuBarItems = 24;

// Window control buttons (minimize / maximize / close) drawn when the custom
// window chrome is on. Not a budget — the set is spelled out in one array in
// ComputeVisibleWindowControlButtons.
inline constexpr std::size_t kWindowControlButtonCount = 3;

// One laid-out menu-bar entry. Menu vocabulary rather than shell state: the
// registry owns MenuId and the cap the container below is sized from, and the
// shell aliases both.
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
// Feature-setting id ("lsp.*.enabled") gating a menu-present LSP action, or empty
// for non-LSP menu actions. Drives hiding the entry when its feature is disabled.
std::string_view LspMenuActionFeatureId(ActionId id);
bool IsLspMenuActionReady(const LspClient::ReadinessSnapshot& snapshot);
// The label an LSP-gated menu entry paints: `ready_label` when the server can answer,
// otherwise that label plus the readiness word the status bar is showing. The returned
// view borrows `scratch` in the not-ready case and the caller's `ready_label` otherwise,
// so it stays valid exactly as long as both do — the menu paints one row at a time and
// reuses a single buffer rather than allocating a label per row per frame.
std::string_view LspDrivenMenuActionLabel(ActionId id,
                                          std::string_view ready_label,
                                          const LspClient::ReadinessSnapshot& snapshot,
                                          std::string& scratch);

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
