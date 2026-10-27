#pragma once

#include <cstddef>

namespace microide::workspace {

// The menus the registry knows, apart from their specs: a status-bar segment that
// opens one names it, and StatusBarService.h must not pull the registry (and the
// LSP client it needs for labels) into every TU that includes it.
enum class MenuId {
  None,
  File,
  Edit,
  View,
  Go,
  Git,
  Remote,
  SidebarMode,
  GitOutgoingBase,
  EditorContext,
  EditorTabContext,
  Terminal,
  Debug,
  Help,
  TerminalContext,
  TerminalTabContext,
  ProjectTabContext,  // last: kMenuIdCount reads it
};

// One slot per MenuId, for tables indexed by it.
inline constexpr std::size_t kMenuIdCount = static_cast<std::size_t>(MenuId::ProjectTabContext) + 1;

// Whether a menu lives on the menu bar, as opposed to opening anchored on a button
// or a click (the sidebar's views menu, the git base menu, the context menus).
constexpr bool IsMenuBarTopLevelMenu(MenuId id) {
  return id != MenuId::SidebarMode && id != MenuId::GitOutgoingBase &&
         id != MenuId::EditorContext && id != MenuId::EditorTabContext &&
         id != MenuId::TerminalContext && id != MenuId::TerminalTabContext &&
         id != MenuId::ProjectTabContext && id != MenuId::None;
}

}  // namespace microide::workspace
