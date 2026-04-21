#pragma once

#include <string_view>

namespace microide::workspace {

enum class ActionId {
  AuthLogin,
  AuthLogout,
  AuthRefresh,
  CodeActions,
  Colorscheme,
  Completion,
  Compare,
  CompareHead,
  DebugStart,
  DebugStop,
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
  InlineCompletion,
  Jump,
  McpTool,
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
  ShowChat,
  ShowOutput,
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
  Tasks,
  TestsDiscover,
  TestsRun,
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
