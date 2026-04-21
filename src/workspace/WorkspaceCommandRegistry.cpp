#include "workspace/WorkspaceCommandRegistry.h"

#include <algorithm>
#include <array>

namespace microide::workspace {

std::span<const ActionSpec> WorkspaceCommandSpecs() {
  static const auto kSpecs = std::to_array<ActionSpec>({
      ActionSpec{ActionId::AuthLogin, "auth-login", "auth-login <provider> [scope...]",
                 "Login", ""},
      ActionSpec{ActionId::AuthLogout, "auth-logout", "auth-logout <provider> <session>",
                 "Logout", ""},
      ActionSpec{ActionId::AuthRefresh, "auth-refresh", "auth-refresh <provider> <session>",
                 "Refresh Session", ""},
      ActionSpec{ActionId::CodeActions, "code-actions", "code-actions", "Code Actions",
                 "Ctrl+."},
      ActionSpec{ActionId::Colorscheme, "colorscheme", "colorscheme [name|list]",
                 "Colorscheme", ""},
      ActionSpec{ActionId::Completion, "completion", "completion", "Completions",
                 "Ctrl+Space"},
      ActionSpec{ActionId::Compare, "compare", "compare [path] [commit-prefix]",
                 "Compare Against...", ""},
      ActionSpec{ActionId::CompareHead, "", "", "Compare Against HEAD", ""},
      ActionSpec{ActionId::DebugStart, "debug-start", "debug-start <type>", "Start Debugger",
                 ""},
      ActionSpec{ActionId::DebugStop, "debug-stop", "debug-stop", "Stop Debugger", ""},
      ActionSpec{ActionId::Merge, "merge", "merge <base> <incoming> <current> [output]",
                 "Merge Editor", ""},
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
      ActionSpec{ActionId::IndentWidth, "indent-width", "indent-width [n]", "Indent Width", ""},
      ActionSpec{ActionId::InlineCompletion, "inline-complete", "inline-complete",
                 "Inline Completion", ""},
      ActionSpec{ActionId::Jump, "jump", "jump <line[:col]>", "Jump Relative", ""},
      ActionSpec{ActionId::McpTool, "mcp", "mcp <tool> [json]", "Run MCP Tool", ""},
      ActionSpec{ActionId::Open, "open", "open <path>", "Open File", ""},
      ActionSpec{ActionId::OpenSelectedTreeItem, "", "", "Open", ""},
      ActionSpec{ActionId::OpenSelectedTreeItemInNewTab, "", "", "Open in New Tab", ""},
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
      ActionSpec{ActionId::Quit, "quit", "quit", "Quit", ""},
      ActionSpec{ActionId::RenamePath, "", "", "Rename...", ""},
      ActionSpec{ActionId::Reopen, "reopen", "reopen", "Reopen", ""},
      ActionSpec{ActionId::Save, "save", "save", "Save", "Ctrl+S"},
      ActionSpec{ActionId::Search, "search", "search <query>", "Find in Buffer", "Ctrl+F"},
      ActionSpec{ActionId::ShowChat, "chat", "chat [message]", "Chat", ""},
      ActionSpec{ActionId::ShowOutput, "output", "output [channel]", "Output", ""},
      ActionSpec{ActionId::SidebarClose, "sidebar-close", "sidebar-close", "Close Sidebar", ""},
      ActionSpec{ActionId::SidebarHide, "sidebar-hide", "sidebar-hide", "Hide Sidebar", ""},
      ActionSpec{ActionId::SidebarShow, "sidebar-show", "sidebar-show [tool]", "Show Sidebar",
                 ""},
      ActionSpec{ActionId::SidebarToggle, "sidebar-toggle", "sidebar-toggle [tool]",
                 "Toggle Sidebar", "F8", true},
      ActionSpec{ActionId::SidebarWidth, "sidebar-width", "sidebar-width <n>", "Sidebar Width",
                 ""},
      ActionSpec{ActionId::SoftTabs, "soft-tabs", "soft-tabs [on|off]", "Soft Tabs", ""},
      ActionSpec{ActionId::SplitFirst, "split-first", "split-first", "First Split", ""},
      ActionSpec{ActionId::SplitLast, "split-last", "split-last", "Last Split", ""},
      ActionSpec{ActionId::SplitNext, "split-next", "split-next", "Next Split", ""},
      ActionSpec{ActionId::SplitPrev, "split-prev", "split-prev", "Previous Split", ""},
      ActionSpec{ActionId::Tab, "tab", "tab [path]", "New Tab", ""},
      ActionSpec{ActionId::TabSize, "tab-size", "tab-size [n]", "Tab Size", ""},
      ActionSpec{ActionId::TabMove, "tabmove", "tabmove <n>", "Move Tab", ""},
      ActionSpec{ActionId::TabSwitch, "tabswitch", "tabswitch <tab>", "Switch Tab", ""},
      ActionSpec{ActionId::Term, "term", "term [command]", "New Terminal", ""},
      ActionSpec{ActionId::Tasks, "tasks", "tasks [task-id]", "Tasks", ""},
      ActionSpec{ActionId::TestsDiscover, "tests-discover", "tests-discover",
                 "Discover Tests", ""},
      ActionSpec{ActionId::TestsRun, "tests-run", "tests-run [test-id...]", "Run Tests", ""},
      ActionSpec{ActionId::Tree, "tree", "tree [root]", "Show Tree", ""},
      ActionSpec{ActionId::TreeRefresh, "tree-refresh", "tree-refresh", "Refresh Tree", ""},
      ActionSpec{ActionId::UiScale, "ui-scale", "ui-scale [n|up|down|reset]", "UI Scale", ""},
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
