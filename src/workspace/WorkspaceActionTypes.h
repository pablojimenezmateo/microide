#pragma once

#include <string_view>

namespace microide::workspace {

enum class ActionId {
  CodeActions,
  Colorscheme,
  Completion,
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
  FindReferences,
  Focus,
  GoToDefinition,
  Goto,
  GitRefresh,
  IndentWidth,
  InlineCompletion,
  Jump,
  Open,
  OpenSelectedTreeItem,
  OpenSelectedTreeItemInNewTab,
  OpenHelpAbout,
  OpenKeyboardShortcuts,
  OpenSettings,
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
  ShowOutput,
  SidebarClose,
  SidebarHide,
  SidebarShow,
  SidebarToggle,
  SidebarWidth,
  SoftTabs,
  Wrap,
  SplitFirst,
  SplitLast,
  SplitNext,
  SplitPrev,
  Tab,
  TabSize,
  TabMove,
  TabSwitch,
  Term,
  TestsDiscover,
  TestsRun,
  Tree,
  TreeRefresh,
  UiScale,
  ToggleLayoutMode,
  ToggleStatusBar,
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
