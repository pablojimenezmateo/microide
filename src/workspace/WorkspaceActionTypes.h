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
  ShowInFileExplorer,
  Save,
  Search,
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
  InsertSnippet,
  SelectAll,
  Undo,
  // Editor essentials: structural & shape actions
  JumpToMatchingBracket,
  ToggleLineComment,
  ToggleBlockComment,
  MoveLineUp,
  MoveLineDown,
  DuplicateLine,
  DeleteLine,
  IndentLines,
  OutdentLines,
  SortLinesAscending,
  SortLinesDescending,
  AddCursorAtNextMatch,
  AddCursorAtAllMatches,
  // Editor essentials: code folding controls
  Fold,
  Unfold,
  FoldAll,
  UnfoldAll,
  ToggleFoldAtCursor,
  // Editor essentials: capability toggles (each must be checkable + command + setting)
  ToggleEditorFolding,
  ToggleEditorStickyScroll,
  ToggleEditorIndentGuides,
  ToggleEditorRenderWhitespace,
  ToggleEditorBracketMatchHighlight,
  ToggleEditorAutoClosePairs,
  ToggleEditorSurround,
  ToggleEditorSmartIndent,
  ToggleEditorToggleComment,
  ToggleEditorLineOps,
  ToggleEditorSortLines,
  ToggleEditorAddCursorAtMatch,
  ToggleEditorOccurrencesHighlight,
  ToggleEditorSearchCaseSensitive,
  ToggleEditorSnippets,
  ToggleEditorSaveTrim,
  ToggleEditorSaveEnsureNewline,
  ToggleEditorAutoDetectIndent,
  MarkBranchFileReviewed,
  MarkBranchHunkReviewed,
  ClearBranchReviewState,
  EditBranchReviewNote,
  // Debugger (DAP). Gated on the `debug.enabled` master toggle.
  StartDebugging,
  StopDebugging,
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

// Returns the WorkspaceSettingsRegistry key backing a ToggleEditor* action, or
// nullptr if the action is not a settings-backed editor-essentials capability
// toggle. Used by both the executor (to flip the setting) and the menu
// rendering path (to derive the checked state).
inline const char* EditorEssentialsCapabilitySettingKey(ActionId id) {
  switch (id) {
    case ActionId::ToggleEditorFolding: return "editor.fold.enabled";
    case ActionId::ToggleEditorStickyScroll: return "editor.fold.sticky_scroll.enabled";
    case ActionId::ToggleEditorIndentGuides: return "editor.view.indent_guides.enabled";
    case ActionId::ToggleEditorRenderWhitespace: return "editor.view.render_whitespace";
    case ActionId::ToggleEditorBracketMatchHighlight:
      return "editor.brackets.match_highlight.enabled";
    case ActionId::ToggleEditorAutoClosePairs: return "editor.brackets.auto_close.enabled";
    case ActionId::ToggleEditorSurround: return "editor.brackets.surround.enabled";
    case ActionId::ToggleEditorSmartIndent: return "editor.indent.smart.enabled";
    case ActionId::ToggleEditorToggleComment: return "editor.shaping.toggle_comment.enabled";
    case ActionId::ToggleEditorLineOps: return "editor.shaping.line_ops.enabled";
    case ActionId::ToggleEditorSortLines: return "editor.shaping.sort_lines.enabled";
    case ActionId::ToggleEditorAddCursorAtMatch:
      return "editor.multicursor.add_at_match.enabled";
    case ActionId::ToggleEditorOccurrencesHighlight: return "editor.occurrences.enabled";
    case ActionId::ToggleEditorSearchCaseSensitive: return "editor.search.case_sensitive";
    case ActionId::ToggleEditorSnippets: return "editor.snippets.enabled";
    case ActionId::ToggleEditorSaveTrim: return "editor.save.trim_trailing_whitespace";
    case ActionId::ToggleEditorSaveEnsureNewline: return "editor.save.ensure_final_newline";
    case ActionId::ToggleEditorAutoDetectIndent: return "editor.indent.detect_on_open";
    default: return nullptr;
  }
}

}  // namespace microide::workspace
