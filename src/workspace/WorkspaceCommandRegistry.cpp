#include "workspace/WorkspaceCommandRegistry.h"

#include <algorithm>
#include <array>

namespace microide::workspace {

std::span<const WorkspaceShell::ActionSpec> WorkspaceCommandSpecs() {
  static const auto kSpecs = std::to_array<WorkspaceShell::ActionSpec>({
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Colorscheme, "colorscheme",
                                 "colorscheme [name|list]", "Colorscheme", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Compare, "compare",
                                 "compare [path] [commit-prefix]", "Compare Against...", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::CompareHead, "", "",
                                 "Compare Against HEAD", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Merge, "merge",
                                 "merge <base> <incoming> <current> [output]",
                                 "Merge Editor", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::CopyAbsolutePath, "", "",
                                 "Copy Absolute Path", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::CopyRelativePath, "", "",
                                 "Copy Relative Path", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::CreateDirectory, "", "",
                                 "New Folder...", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::CreateFile, "", "", "New File...", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::DeletePath, "", "", "Delete...", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Files, "files", "files [root]",
                                 "Find File", "F6"},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Find, "find", "find <query>",
                                 "Find File By Query", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Focus, "focus",
                                 "focus <editor|sidebar|panel>", "Focus", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Goto, "goto", "goto <line[:col]>",
                                 "Go to Line", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::GitRefresh, "git-refresh",
                                 "git-refresh", "Refresh Git", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::IndentWidth, "indent-width",
                                 "indent-width [n]", "Indent Width", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Jump, "jump", "jump <line[:col]>",
                                 "Jump Relative", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Open, "open", "open <path>",
                                 "Open File", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::OpenSelectedTreeItem, "", "", "Open",
                                 ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::OpenSelectedTreeItemInNewTab, "", "",
                                 "Open in New Tab", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::ProjectClose, "project-close",
                                 "project-close", "Close Project", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::ProjectNext, "project-next",
                                 "project-next", "Next Project", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::ProjectOpen, "project-open",
                                 "project-open [path]", "Open Project", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::ProjectPrev, "project-prev",
                                 "project-prev", "Previous Project", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::ProjectSearch, "project-search",
                                 "project-search [query]", "Find in Project",
                                 "Ctrl+Shift+F"},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::PluginsReload, "plugins-reload",
                                 "plugins-reload", "Reload Plugins", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Quit, "quit", "quit", "Quit", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::RenamePath, "", "", "Rename...", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Reopen, "reopen", "reopen", "Reopen",
                                 ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Save, "save", "save", "Save",
                                 "Ctrl+S"},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Search, "search", "search <query>",
                                 "Find in Buffer", "Ctrl+F"},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::SidebarClose, "sidebar-close",
                                 "sidebar-close", "Close Sidebar", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::SidebarHide, "sidebar-hide",
                                 "sidebar-hide", "Hide Sidebar", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::SidebarShow, "sidebar-show",
                                 "sidebar-show [tool]", "Show Sidebar", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::SidebarToggle, "sidebar-toggle",
                                 "sidebar-toggle [tool]", "Toggle Sidebar", "F8", true},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::SidebarWidth, "sidebar-width",
                                 "sidebar-width <n>", "Sidebar Width", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::SoftTabs, "soft-tabs",
                                 "soft-tabs [on|off]", "Soft Tabs", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::SplitFirst, "split-first",
                                 "split-first", "First Split", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::SplitLast, "split-last",
                                 "split-last", "Last Split", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::SplitNext, "split-next",
                                 "split-next", "Next Split", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::SplitPrev, "split-prev",
                                 "split-prev", "Previous Split", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Tab, "tab", "tab [path]",
                                 "New Tab", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::TabSize, "tab-size",
                                 "tab-size [n]", "Tab Size", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::TabMove, "tabmove", "tabmove <n>",
                                 "Move Tab", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::TabSwitch, "tabswitch",
                                 "tabswitch <tab>", "Switch Tab", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Term, "term", "term [command]",
                                 "New Terminal", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Tree, "tree", "tree [root]",
                                 "Show Tree", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::TreeRefresh, "tree-refresh",
                                 "tree-refresh", "Refresh Tree", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::UiScale, "ui-scale",
                                 "ui-scale [n|up|down|reset]", "UI Scale", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Unsplit, "unsplit", "unsplit",
                                 "Close Split", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Vsplit, "vsplit", "vsplit [path]",
                                 "Split Right", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::CloseActiveTab, "", "",
                                 "Close Tab", "Ctrl+W"},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::CloseAllTabs, "", "",
                                 "Close All Tabs", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::CloseOtherTabs, "", "",
                                 "Close Other Tabs", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::CloseTabsToRight, "", "",
                                 "Close Tabs to the Right", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::CloseTabsToLeft, "", "",
                                 "Close Tabs to the Left", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::CopyLastTerminalCommand, "", "",
                                 "Copy Last Command + Output", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::CopySelection, "", "", "Copy",
                                 "Ctrl+C"},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::CopySelectionWithContext, "", "",
                                 "Copy with Context", ""},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::CutSelection, "", "", "Cut",
                                 "Ctrl+X"},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::OpenCommandPrompt, "", "",
                                 "Command Prompt", "Ctrl+E"},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::PasteClipboard, "", "", "Paste",
                                 "Ctrl+V"},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Redo, "", "", "Redo",
                                 "Ctrl+Y / Ctrl+Shift+Z"},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::ReplaceInBuffer, "", "",
                                 "Replace in Buffer", "Ctrl+H"},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::SelectAll, "", "", "Select All",
                                 "Ctrl+A"},
      WorkspaceShell::ActionSpec{WorkspaceShell::ActionId::Undo, "", "", "Undo", "Ctrl+Z"},
  });
  return kSpecs;
}

const WorkspaceShell::ActionSpec* FindWorkspaceActionSpec(WorkspaceShell::ActionId id) {
  const auto specs = WorkspaceCommandSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(),
                               [id](const WorkspaceShell::ActionSpec& spec) {
                                 return spec.id == id;
                               });
  return it == specs.end() ? nullptr : &(*it);
}

const WorkspaceShell::ActionSpec* FindWorkspaceActionByCommand(std::string_view command_name) {
  const auto specs = WorkspaceCommandSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(),
                               [command_name](const WorkspaceShell::ActionSpec& spec) {
                                 return !spec.command_name.empty() &&
                                        spec.command_name == command_name;
                               });
  return it == specs.end() ? nullptr : &(*it);
}

const std::vector<std::string>& WorkspaceCommandNames() {
  static const std::vector<std::string> kNames = [] {
    std::vector<std::string> names;
    for (const WorkspaceShell::ActionSpec& spec : WorkspaceCommandSpecs()) {
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
  for (const WorkspaceShell::ActionSpec& spec : WorkspaceCommandSpecs()) {
    if (spec.command_name.empty()) {
      continue;
    }
    usages.push_back(spec.command_usage.empty() ? std::string(spec.command_name)
                                                : std::string(spec.command_usage));
  }
  return usages;
}

}  // namespace microide::workspace
