#pragma once

#include <string_view>

namespace microide::workspace {

enum class ActionId {
  Colorscheme,
  Compare,
  CompareHead,
  Merge,
  CopyAbsolutePath,
  CopyRelativePath,
  CreateDirectory,
  CreateFile,
  DeletePath,
  Files,
  Find,
  Focus,
  Goto,
  GitRefresh,
  IndentWidth,
  Jump,
  Open,
  OpenSelectedTreeItem,
  OpenSelectedTreeItemInNewTab,
  ProjectClose,
  ProjectNext,
  ProjectOpen,
  ProjectPrev,
  ProjectSearch,
  PluginsReload,
  Quit,
  RenamePath,
  Reopen,
  Save,
  Search,
  SidebarClose,
  SidebarHide,
  SidebarShow,
  SidebarToggle,
  SidebarWidth,
  SoftTabs,
  SplitFirst,
  SplitLast,
  SplitNext,
  SplitPrev,
  Tab,
  TabSize,
  TabMove,
  TabSwitch,
  Term,
  Tree,
  TreeRefresh,
  UiScale,
  Unsplit,
  Vsplit,
  CloseActiveTab,
  CloseAllTabs,
  CloseOtherTabs,
  CloseTabsToRight,
  CloseTabsToLeft,
  CopyLastTerminalCommand,
  CopySelection,
  CopySelectionWithContext,
  CutSelection,
  OpenCommandPrompt,
  PasteClipboard,
  Redo,
  ReplaceInBuffer,
  SelectAll,
  Undo,
};

enum class ActionSource {
  Command,
  Shortcut,
  Menu,
  ContextMenu,
};

struct ActionSpec {
  ActionId id;
  std::string_view command_name;
  std::string_view command_usage;
  std::string_view label;
  std::string_view accelerator;
  bool checkable = false;
};

}  // namespace microide::workspace
