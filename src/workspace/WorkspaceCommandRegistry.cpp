#include "workspace/WorkspaceCommandRegistry.h"

#include <algorithm>
#include <array>

namespace microide::workspace {

std::span<const ActionSpec> WorkspaceCommandSpecs() {
  static const auto kSpecs = std::to_array<ActionSpec>({
      ActionSpec{ActionId::CodeActions, "code-actions", "code-actions", "Code Actions",
                 "Ctrl+."},
      ActionSpec{ActionId::Colorscheme, "colorscheme", "colorscheme [name|list]",
                 "Colorscheme", ""},
      ActionSpec{ActionId::Completion, "completion", "completion", "Completions",
                 "Ctrl+Space"},
      ActionSpec{ActionId::InsertSnippet, "insert-snippet", "insert-snippet", "Insert Snippet…",
                 "Ctrl+Alt+J"},
      ActionSpec{ActionId::Compare, "compare", "compare [path] [commit-prefix]",
                 "Compare Against...", ""},
      ActionSpec{ActionId::CompareHead, "", "", "Compare Against HEAD", ""},
      ActionSpec{ActionId::Merge, "merge", "merge <base> <incoming> <current> [output]",
                 "Merge Editor", ""},
      ActionSpec{ActionId::ShowInFileExplorer, "", "", "Show in File Explorer", ""},
      ActionSpec{ActionId::CopyAbsolutePath, "", "", "Copy Absolute Path", ""},
      ActionSpec{ActionId::CopyRelativePath, "", "", "Copy Relative Path", ""},
      ActionSpec{ActionId::CreateDirectory, "", "", "New Folder...", ""},
      ActionSpec{ActionId::CreateFile, "", "", "New File...", ""},
      ActionSpec{ActionId::DeletePath, "", "", "Delete...", ""},
      ActionSpec{ActionId::Files, "files", "files [root]", "Find File", "F6"},
      ActionSpec{ActionId::Find, "find", "find <query>", "Find File By Query", ""},
      ActionSpec{ActionId::FindReferences, "find-references", "find-references",
                 "Find References", "Shift+F12"},
      ActionSpec{ActionId::Focus, "focus", "focus <editor|sidebar|panel>", "Focus", ""},
      ActionSpec{ActionId::GoToDefinition, "goto-definition", "goto-definition",
                 "Go to Definition", "F12"},
      ActionSpec{ActionId::Goto, "goto", "goto <line[:col]>", "Go to Line", ""},
      ActionSpec{ActionId::GitRefresh, "git-refresh", "git-refresh", "Refresh Git", ""},
      ActionSpec{ActionId::MarkBranchFileReviewed, "mark-branch-file-reviewed",
                 "mark-branch-file-reviewed", "Mark Branch File Reviewed", ""},
      ActionSpec{ActionId::MarkBranchHunkReviewed, "mark-branch-hunk-reviewed",
                 "mark-branch-hunk-reviewed", "Mark Branch Hunk Reviewed", ""},
      ActionSpec{ActionId::ClearBranchReviewState, "clear-branch-review-state",
                 "clear-branch-review-state", "Clear Branch Review State", ""},
      ActionSpec{ActionId::EditBranchReviewNote, "branch-review-note",
                 "branch-review-note <text>", "Edit Branch Review Note", ""},
      ActionSpec{ActionId::IndentWidth, "indent-width", "indent-width [n]", "Indent Width", ""},
      ActionSpec{ActionId::Jump, "jump", "jump <line[:col]>", "Jump Relative", ""},
      ActionSpec{ActionId::Open, "open", "open <path>", "Open File", ""},
      ActionSpec{ActionId::OpenSelectedTreeItem, "", "", "Open", ""},
      ActionSpec{ActionId::OpenSelectedTreeItemInNewTab, "", "", "Open in New Tab", ""},
      ActionSpec{ActionId::OpenHelpAbout, "about", "about", "About microide", ""},
      ActionSpec{ActionId::OpenKeyboardShortcuts, "keyboard-shortcuts", "keyboard-shortcuts",
                 "Keyboard Shortcuts", ""},
      ActionSpec{ActionId::OpenSettings, "settings", "settings", "Settings", "Ctrl+,"},
      ActionSpec{ActionId::ProjectClose, "project-close", "project-close", "Close Project", ""},
      ActionSpec{ActionId::ProjectNext, "project-next", "project-next", "Next Project", ""},
      ActionSpec{ActionId::ProjectOpen, "project-open", "project-open [path]", "Open Project",
                 ""},
      ActionSpec{ActionId::ProjectPrev, "project-prev", "project-prev", "Previous Project", ""},
      ActionSpec{ActionId::ProjectSearch, "project-search", "project-search [query]",
                 "Find in Project",
                                 "Ctrl+Shift+F"},
      ActionSpec{ActionId::PluginsReload, "plugins-reload", "plugins-reload", "Reload Plugins",
                 ""},
      ActionSpec{ActionId::StartDebugging, "debug-start", "debug-start", "Start Debugging", ""},
      ActionSpec{ActionId::StopDebugging, "debug-stop", "debug-stop", "Stop Debugging", ""},
      ActionSpec{ActionId::DebugContinue, "debug-continue", "debug-continue", "Continue", "F5"},
      ActionSpec{ActionId::DebugStepOver, "debug-step-over", "debug-step-over", "Step Over", "F10"},
      ActionSpec{ActionId::DebugStepIn, "debug-step-in", "debug-step-in", "Step In", "F11"},
      ActionSpec{ActionId::DebugStepOut, "debug-step-out", "debug-step-out", "Step Out",
                 "Shift+F11"},
      ActionSpec{ActionId::DebugPause, "debug-pause", "debug-pause", "Pause", ""},
      ActionSpec{ActionId::DebugRestart, "debug-restart", "debug-restart", "Restart Debugging",
                 "Ctrl+Shift+F5"},
      ActionSpec{ActionId::DebugSwitchSession, "debug-switch-session", "debug-switch-session [n]",
                 "Switch Debug Session", ""},
      ActionSpec{ActionId::DebugStopAllSessions, "debug-stop-all", "debug-stop-all",
                 "Stop All Debug Sessions", ""},
      ActionSpec{ActionId::DebugConsoleRepl, "debug-repl", "debug-repl", "Evaluate in Debug Console",
                 ""},
      ActionSpec{ActionId::PickLaunchConfig, "debug-pick-config", "debug-pick-config",
                 "Select Launch Configuration", ""},
      ActionSpec{ActionId::DebugPaneToggle, "debug-pane-toggle", "debug-pane-toggle",
                 "Toggle Debug Pane", "Ctrl+Shift+D", true},
      ActionSpec{ActionId::DebugPaneShowCallStack, "debug-pane-callstack", "debug-pane-callstack",
                 "Show Call Stack", ""},
      ActionSpec{ActionId::DebugPaneShowVariables, "debug-pane-variables", "debug-pane-variables",
                 "Show Variables", ""},
      ActionSpec{ActionId::DebugPaneShowWatch, "debug-pane-watch", "debug-pane-watch",
                 "Show Watch", ""},
      ActionSpec{ActionId::DebugPaneShowBreakpoints, "debug-pane-breakpoints",
                 "debug-pane-breakpoints", "Show Breakpoints", ""},
      ActionSpec{ActionId::Quit, "quit", "quit", "Quit", ""},
      ActionSpec{ActionId::RenamePath, "", "", "Rename...", ""},
      ActionSpec{ActionId::Reopen, "reopen", "reopen", "Reopen", ""},
      ActionSpec{ActionId::Save, "save", "save", "Save", "Ctrl+S"},
      ActionSpec{ActionId::Search, "search", "search <query>", "Find in Buffer", "Ctrl+F"},
      ActionSpec{ActionId::SidebarClose, "sidebar-close", "sidebar-close", "Close Sidebar", ""},
      ActionSpec{ActionId::SidebarHide, "sidebar-hide", "sidebar-hide", "Hide Sidebar", ""},
      ActionSpec{ActionId::SidebarShow, "sidebar-show", "sidebar-show [tool]", "Show Sidebar",
                 ""},
      ActionSpec{ActionId::SidebarToggle, "sidebar-toggle", "sidebar-toggle [tool]",
                 "Toggle Sidebar", "F8", true},
      ActionSpec{ActionId::SidebarWidth, "sidebar-width", "sidebar-width <n>", "Sidebar Width",
                 ""},
      ActionSpec{ActionId::SoftTabs, "soft-tabs", "soft-tabs [on|off]", "Soft Tabs", ""},
      ActionSpec{ActionId::Wrap, "wrap", "wrap [on|off]", "Word Wrap", "", true},
      ActionSpec{ActionId::SplitFirst, "split-first", "split-first", "First Split", ""},
      ActionSpec{ActionId::SplitLast, "split-last", "split-last", "Last Split", ""},
      ActionSpec{ActionId::SplitNext, "split-next", "split-next", "Next Split", ""},
      ActionSpec{ActionId::SplitPrev, "split-prev", "split-prev", "Previous Split", ""},
      ActionSpec{ActionId::Tab, "tab", "tab [path]", "New Tab", ""},
      ActionSpec{ActionId::TabSize, "tab-size", "tab-size [n]", "Tab Size", ""},
      ActionSpec{ActionId::TabMove, "tabmove", "tabmove <n>", "Move Tab", ""},
      ActionSpec{ActionId::TabSwitch, "tabswitch", "tabswitch <tab>", "Switch Tab", ""},
      ActionSpec{ActionId::Term, "term", "term [command]", "New Terminal", ""},
      ActionSpec{ActionId::TestsDiscover, "tests-discover", "tests-discover",
                 "Discover Tests", ""},
      ActionSpec{ActionId::TestsRun, "tests-run", "tests-run [test-id...]", "Run Tests", ""},
      ActionSpec{ActionId::Tree, "tree", "tree [root]", "Show Tree", ""},
      ActionSpec{ActionId::TreeRefresh, "tree-refresh", "tree-refresh", "Refresh Tree", ""},
      ActionSpec{ActionId::UiScale, "ui-scale", "ui-scale [n|up|down|reset]", "UI Scale", ""},
      ActionSpec{ActionId::ToggleLayoutMode, "layout-mode-toggle", "layout-mode-toggle",
                 "Toggle Layout Mode", ""},
      ActionSpec{ActionId::ToggleStatusBar, "status-bar-toggle", "status-bar-toggle",
                 "Status Bar", "", true},
      ActionSpec{ActionId::Unsplit, "unsplit", "unsplit", "Close Split", ""},
      ActionSpec{ActionId::Vsplit, "vsplit", "vsplit [path]", "Split Right", ""},
      ActionSpec{ActionId::CloseActiveTab, "", "", "Close Tab", "Ctrl+W"},
      ActionSpec{ActionId::CloseAllTabs, "", "", "Close All Tabs", ""},
      ActionSpec{ActionId::CloseOtherTabs, "", "", "Close Other Tabs", ""},
      ActionSpec{ActionId::CloseTabsToRight, "", "", "Close Tabs to the Right", ""},
      ActionSpec{ActionId::CloseTabsToLeft, "", "", "Close Tabs to the Left", ""},
      ActionSpec{ActionId::CopyLastTerminalCommand, "", "", "Copy Last Command + Output", ""},
      ActionSpec{ActionId::CopySelection, "", "", "Copy", "Ctrl+C"},
      ActionSpec{ActionId::CopySelectionWithContext, "", "", "Copy with Context", ""},
      ActionSpec{ActionId::CutSelection, "", "", "Cut", "Ctrl+X"},
      ActionSpec{ActionId::OpenCommandPrompt, "", "", "Command Prompt", "Ctrl+E"},
      ActionSpec{ActionId::PasteClipboard, "", "", "Paste", "Ctrl+V"},
      ActionSpec{ActionId::Redo, "", "", "Redo", "Ctrl+Y / Ctrl+Shift+Z"},
      ActionSpec{ActionId::ReplaceInBuffer, "", "", "Replace in Buffer", "Ctrl+H"},
      ActionSpec{ActionId::SelectAll, "", "", "Select All", "Ctrl+A"},
      ActionSpec{ActionId::Undo, "", "", "Undo", "Ctrl+Z"},
      ActionSpec{ActionId::JumpToMatchingBracket, "jump-to-matching-bracket",
                 "jump-to-matching-bracket", "Jump to Matching Bracket", "Ctrl+Shift+\\"},
      ActionSpec{ActionId::ToggleLineComment, "toggle-line-comment", "toggle-line-comment",
                 "Toggle Line Comment", "Ctrl+/"},
      ActionSpec{ActionId::ToggleBlockComment, "toggle-block-comment", "toggle-block-comment",
                 "Toggle Block Comment", "Shift+Alt+A"},
      ActionSpec{ActionId::MoveLineUp, "move-line-up", "move-line-up", "Move Line Up", "Alt+Up"},
      ActionSpec{ActionId::MoveLineDown, "move-line-down", "move-line-down", "Move Line Down",
                 "Alt+Down"},
      ActionSpec{ActionId::DuplicateLine, "duplicate-line", "duplicate-line",
                 "Duplicate Selection / Line", "Shift+Alt+Down"},
      ActionSpec{ActionId::DeleteLine, "delete-line", "delete-line", "Delete Line",
                 "Ctrl+Shift+K"},
      ActionSpec{ActionId::IndentLines, "indent-lines", "indent-lines", "Indent Lines", "Tab"},
      ActionSpec{ActionId::OutdentLines, "outdent-lines", "outdent-lines", "Outdent Lines",
                 "Shift+Tab"},
      ActionSpec{ActionId::SortLinesAscending, "sort-lines-ascending", "sort-lines-ascending",
                 "Sort Lines Ascending", ""},
      ActionSpec{ActionId::SortLinesDescending, "sort-lines-descending", "sort-lines-descending",
                 "Sort Lines Descending", ""},
      ActionSpec{ActionId::AddCursorAtNextMatch, "add-cursor-next-match", "add-cursor-next-match",
                 "Add Cursor at Next Match", "Ctrl+D"},
      ActionSpec{ActionId::AddCursorAtAllMatches, "add-cursor-all-matches",
                 "add-cursor-all-matches", "Add Cursor at All Matches", "Ctrl+Shift+L"},
      ActionSpec{ActionId::Fold, "fold", "fold", "Fold", "Ctrl+Shift+["},
      ActionSpec{ActionId::Unfold, "unfold", "unfold", "Unfold", "Ctrl+Shift+]"},
      ActionSpec{ActionId::FoldAll, "fold-all", "fold-all", "Fold All", "Ctrl+K Ctrl+0"},
      ActionSpec{ActionId::UnfoldAll, "unfold-all", "unfold-all", "Unfold All", "Ctrl+K Ctrl+J"},
      ActionSpec{ActionId::ToggleFoldAtCursor, "toggle-fold", "toggle-fold",
                 "Toggle Fold at Cursor", ""},
      ActionSpec{ActionId::ToggleEditorFolding, "toggle-editor-folding", "toggle-editor-folding",
                 "Code Folding", "", true},
      ActionSpec{ActionId::ToggleEditorStickyScroll, "toggle-editor-sticky-scroll",
                 "toggle-editor-sticky-scroll", "Sticky Scroll", "", true},
      ActionSpec{ActionId::ToggleEditorIndentGuides, "toggle-editor-indent-guides",
                 "toggle-editor-indent-guides", "Indent Guides", "", true},
      ActionSpec{ActionId::ToggleEditorRenderWhitespace, "toggle-editor-render-whitespace",
                 "toggle-editor-render-whitespace", "Render Whitespace", "", true},
      ActionSpec{ActionId::ToggleEditorBracketMatchHighlight,
                 "toggle-editor-bracket-match-highlight",
                 "toggle-editor-bracket-match-highlight", "Bracket Match Highlight", "", true},
      ActionSpec{ActionId::ToggleEditorAutoClosePairs, "toggle-editor-auto-close",
                 "toggle-editor-auto-close", "Auto-Close Pairs", "", true},
      ActionSpec{ActionId::ToggleEditorSurround, "toggle-editor-surround",
                 "toggle-editor-surround", "Surround Selection", "", true},
      ActionSpec{ActionId::ToggleEditorSmartIndent, "toggle-editor-smart-indent",
                 "toggle-editor-smart-indent", "Smart Indent", "", true},
      ActionSpec{ActionId::ToggleEditorToggleComment, "toggle-editor-toggle-comment",
                 "toggle-editor-toggle-comment", "Toggle Comment Action", "", true},
      ActionSpec{ActionId::ToggleEditorLineOps, "toggle-editor-line-ops",
                 "toggle-editor-line-ops", "Line Move/Duplicate/Delete", "", true},
      ActionSpec{ActionId::ToggleEditorSortLines, "toggle-editor-sort-lines",
                 "toggle-editor-sort-lines", "Sort Lines Action", "", true},
      ActionSpec{ActionId::ToggleEditorAddCursorAtMatch, "toggle-editor-add-cursor-at-match",
                 "toggle-editor-add-cursor-at-match", "Add Cursor at Match", "", true},
      ActionSpec{ActionId::ToggleEditorOccurrencesHighlight,
                 "toggle-editor-occurrences-highlight",
                 "toggle-editor-occurrences-highlight", "Occurrences Highlight", "", true},
      ActionSpec{ActionId::ToggleEditorSearchCaseSensitive,
                 "toggle-editor-search-case-sensitive",
                 "toggle-editor-search-case-sensitive", "Case-Sensitive Search Seeds", "", true},
      ActionSpec{ActionId::ToggleEditorSnippets, "toggle-editor-snippets",
                 "toggle-editor-snippets", "Snippets", "", true},
      ActionSpec{ActionId::ToggleEditorSaveTrim, "toggle-editor-save-trim",
                 "toggle-editor-save-trim", "Trim Trailing Whitespace on Save", "", true},
      ActionSpec{ActionId::ToggleEditorSaveEnsureNewline, "toggle-editor-save-ensure-newline",
                 "toggle-editor-save-ensure-newline", "Ensure Final Newline on Save", "", true},
      ActionSpec{ActionId::ToggleEditorAutoDetectIndent, "toggle-editor-auto-detect-indent",
                 "toggle-editor-auto-detect-indent", "Auto-Detect Indent on Open", "", true},
  });
  return kSpecs;
}

const ActionSpec* FindWorkspaceActionSpec(ActionId id) {
  const auto specs = WorkspaceCommandSpecs();
  const auto it =
      std::find_if(specs.begin(), specs.end(), [id](const ActionSpec& spec) { return spec.id == id; });
  return it == specs.end() ? nullptr : &(*it);
}

const ActionSpec* FindWorkspaceActionByCommand(std::string_view command_name) {
  const auto specs = WorkspaceCommandSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(), [command_name](const ActionSpec& spec) {
    return !spec.command_name.empty() && spec.command_name == command_name;
  });
  return it == specs.end() ? nullptr : &(*it);
}

const std::vector<std::string>& WorkspaceCommandNames() {
  static const std::vector<std::string> kNames = [] {
    std::vector<std::string> names;
    for (const ActionSpec& spec : WorkspaceCommandSpecs()) {
      if (!spec.command_name.empty()) {
        names.emplace_back(spec.command_name);
      }
    }
    return names;
  }();
  return kNames;
}

std::vector<std::string> WorkspaceDocumentedCommandUsages() {
  std::vector<std::string> usages;
  for (const ActionSpec& spec : WorkspaceCommandSpecs()) {
    if (spec.command_name.empty()) {
      continue;
    }
    usages.push_back(spec.command_usage.empty() ? std::string(spec.command_name)
                                                : std::string(spec.command_usage));
  }
  return usages;
}

}  // namespace microide::workspace
