#pragma once

#include <cstddef>

namespace microide::workspace {

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
  ProjectTabContext,
};

inline constexpr std::size_t kMenuIdCount = static_cast<std::size_t>(MenuId::ProjectTabContext) + 1;

constexpr bool IsMenuBarTopLevelMenu(MenuId id) {
  return id != MenuId::SidebarMode && id != MenuId::GitOutgoingBase &&
         id != MenuId::EditorContext && id != MenuId::EditorTabContext &&
         id != MenuId::TerminalContext && id != MenuId::TerminalTabContext &&
         id != MenuId::ProjectTabContext && id != MenuId::None;
}

}
