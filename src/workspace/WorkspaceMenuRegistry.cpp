#include "workspace/WorkspaceMenuRegistry.h"

#include <algorithm>
#include <array>

#include "plugin/PluginHost.h"

namespace microide::workspace {

namespace {

MenuItemSpec MenuItem(ActionId action,
                      std::string_view label = {},
                      std::string_view accelerator = {},
                      std::array<std::string_view, 2> args = {},
                      std::size_t arg_count = 0,
                      bool checkable = false,
                      MenuId submenu = MenuId::None) {
  return MenuItemSpec{action, label, accelerator, args, arg_count, false, checkable, submenu, {}};
}

MenuItemSpec MenuSeparator() {
  return MenuItemSpec{
      ActionId::Colorscheme, {}, {}, {}, 0, true, false, MenuId::None, {}};
}

std::string LspReadinessSuffix(const LspClient::ReadinessSnapshot& snapshot) {
  using State = LspClient::ReadinessSnapshot::State;
  switch (snapshot.state) {
    case State::Idle:
      return "No LSP";
    case State::Starting:
      return "LSP starting...";
    case State::Indexing:
      return snapshot.indexed_count > 0
                 ? "LSP indexing " + std::to_string(snapshot.indexed_count) + "..."
                 : "LSP indexing...";
    case State::Ready:
      return {};
    case State::Failed:
      return "LSP failed";
  }
  return {};
}

}  // namespace

bool IsLspDrivenMenuAction(ActionId id) {
  return id == ActionId::GoToDefinition || id == ActionId::FindReferences;
}

bool IsLspMenuActionReady(const LspClient::ReadinessSnapshot& snapshot) {
  return snapshot.state == LspClient::ReadinessSnapshot::State::Ready;
}

std::string LspDrivenMenuActionLabel(ActionId id,
                                     std::string_view ready_label,
                                     const LspClient::ReadinessSnapshot& snapshot) {
  if (!IsLspDrivenMenuAction(id) || IsLspMenuActionReady(snapshot)) {
    return std::string(ready_label);
  }

  std::string label(ready_label);
  const std::string suffix = LspReadinessSuffix(snapshot);
  if (!suffix.empty()) {
    label += " (";
    label += suffix;
    label += ")";
  }
  return label;
}

std::span<const MenuSpec> WorkspaceMenuSpecs() {
  static const auto kFileItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::Tab, "New File", "Ctrl+N"),
      MenuItem(ActionId::Open, "Open File…", "Ctrl+O"),
      MenuItem(ActionId::ProjectOpen, "Open Folder / Project Tab…", "Ctrl+K Ctrl+O"),
      MenuSeparator(),
      MenuItem(ActionId::Save),
      MenuItem(ActionId::CloseActiveTab),
      MenuItem(ActionId::CloseAllTabs),
      MenuItem(ActionId::Reopen),
      MenuSeparator(),
      MenuItem(ActionId::ProjectClose),
      MenuSeparator(),
      MenuItem(ActionId::Quit),
  });
  static const auto kEditItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::Undo),
      MenuItem(ActionId::Redo),
      MenuSeparator(),
      MenuItem(ActionId::CutSelection),
      MenuItem(ActionId::CopySelection),
      MenuItem(ActionId::CopySelectionWithContext),
      MenuItem(ActionId::PasteClipboard),
      MenuItem(ActionId::SelectAll),
      MenuSeparator(),
      MenuItem(ActionId::ToggleLineComment, "Toggle Line Comment", "Ctrl+/"),
      MenuItem(ActionId::ToggleBlockComment, "Toggle Block Comment", "Shift+Alt+A"),
      MenuItem(ActionId::JumpToMatchingBracket, "Jump to Matching Bracket", "Ctrl+Shift+\\"),
      MenuSeparator(),
      MenuItem(ActionId::MoveLineUp, "Move Line Up", "Alt+Up"),
      MenuItem(ActionId::MoveLineDown, "Move Line Down", "Alt+Down"),
      MenuItem(ActionId::DuplicateLine, "Duplicate Line", "Shift+Alt+Down"),
      MenuItem(ActionId::DeleteLine, "Delete Line", "Ctrl+Shift+K"),
      MenuItem(ActionId::IndentLines, "Indent Lines", "Tab"),
      MenuItem(ActionId::OutdentLines, "Outdent Lines", "Shift+Tab"),
      MenuSeparator(),
      MenuItem(ActionId::SortLinesAscending, "Sort Lines Ascending"),
      MenuItem(ActionId::SortLinesDescending, "Sort Lines Descending"),
      MenuSeparator(),
      MenuItem(ActionId::Fold, "Fold", "Ctrl+Shift+["),
      MenuItem(ActionId::Unfold, "Unfold", "Ctrl+Shift+]"),
      MenuItem(ActionId::ToggleFoldAtCursor, "Toggle Fold at Cursor"),
      MenuItem(ActionId::FoldAll, "Fold All", "Ctrl+K Ctrl+0"),
      MenuItem(ActionId::UnfoldAll, "Unfold All", "Ctrl+K Ctrl+J"),
      MenuSeparator(),
      MenuItem(ActionId::Completion),
      MenuItem(ActionId::InsertSnippet),
      MenuItem(ActionId::CodeActions),
      MenuItem(ActionId::GoToDefinition),
      MenuItem(ActionId::FindReferences),
  });
  static const auto kEditorContextItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::Undo),
      MenuItem(ActionId::Redo),
      MenuSeparator(),
      MenuItem(ActionId::CutSelection),
      MenuItem(ActionId::CopySelection),
      MenuItem(ActionId::CopySelectionWithContext),
      MenuItem(ActionId::PasteClipboard),
      MenuItem(ActionId::SelectAll),
      MenuSeparator(),
      MenuItem(ActionId::Completion),
      MenuItem(ActionId::InsertSnippet),
      MenuItem(ActionId::CodeActions),
      MenuItem(ActionId::GoToDefinition),
      MenuItem(ActionId::FindReferences),
  });
  static const auto kViewItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::SidebarToggle, {}, {}, {}, 0, true),
      MenuSeparator(),
      MenuItem(ActionId::Wrap, "Word Wrap", {}, {}, 0, true),
      MenuItem(ActionId::ToggleStatusBar, "Status Bar", {}, {}, 0, true),
      MenuSeparator(),
      MenuItem(ActionId::ToggleEditorFolding, "Code Folding", {}, {}, 0, true),
      MenuItem(ActionId::ToggleEditorStickyScroll, "Sticky Scroll", {}, {}, 0, true),
      MenuItem(ActionId::ToggleEditorIndentGuides, "Indent Guides", {}, {}, 0, true),
      MenuItem(ActionId::ToggleEditorRenderWhitespace, "Render Whitespace", {}, {}, 0, true),
      MenuItem(ActionId::ToggleEditorBracketMatchHighlight, "Bracket Match Highlight", {}, {}, 0,
               true),
      MenuItem(ActionId::ToggleEditorOccurrencesHighlight, "Occurrences Highlight", {}, {}, 0,
               true),
      MenuSeparator(),
      MenuItem(ActionId::UiScale, "Zoom In", "Ctrl+=", std::array<std::string_view, 2>{"up", {}},
               1),
      MenuItem(ActionId::UiScale, "Zoom Out", "Ctrl+-",
               std::array<std::string_view, 2>{"down", {}}, 1),
      MenuItem(ActionId::UiScale, "Reset Zoom", "Ctrl+0",
               std::array<std::string_view, 2>{"reset", {}}, 1),
  });
  static const auto kSelectionItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::SelectAll),
      MenuItem(ActionId::CutSelection),
      MenuItem(ActionId::CopySelection),
      MenuItem(ActionId::CopySelectionWithContext),
      MenuItem(ActionId::PasteClipboard),
      MenuSeparator(),
      MenuItem(ActionId::AddCursorAtNextMatch, "Add Cursor at Next Match", "Ctrl+D"),
      MenuItem(ActionId::AddCursorAtAllMatches, "Add Cursor at All Matches", "Ctrl+Shift+L"),
      MenuItem(ActionId::ToggleEditorAddCursorAtMatch, "Add Cursor Action", {}, {}, 0, true),
      MenuItem(ActionId::ToggleEditorOccurrencesHighlight, "Highlight Occurrences", {}, {}, 0,
               true),
      MenuItem(ActionId::ToggleEditorSearchCaseSensitive, "Case-Sensitive Search Seeds", {}, {}, 0,
               true),
  });
  static const auto kGoItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::Files, "Go to File…"),
      MenuItem(ActionId::ProjectSearch, "Search in Project…"),
      MenuSeparator(),
      MenuItem(ActionId::Goto, "Go to Line…"),
      MenuItem(ActionId::Jump, "Jump Relative…"),
      MenuSeparator(),
      MenuItem(ActionId::GoToDefinition),
      MenuItem(ActionId::FindReferences),
  });
  static const auto kGitItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::SidebarShow, "Source Control Sidebar", {},
               std::array<std::string_view, 2>{"git", {}}, 1, false),
      MenuItem(ActionId::GitRefresh, "Refresh"),
      MenuSeparator(),
      MenuItem(ActionId::CompareHead, "Compare with HEAD"),
  });
  static const auto kTerminalItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::Term, "New Terminal"),
      MenuItem(ActionId::CopyLastTerminalCommand, "Copy Last Command"),
  });
  static const auto kPreferencesItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::OpenSettings, "Settings…", "Ctrl+,"),
      MenuItem(ActionId::ToggleLayoutMode, "Compact mode", {}, {}, 0, true),
      MenuSeparator(),
      MenuItem(ActionId::ToggleEditorAutoClosePairs, "Auto-Close Pairs", {}, {}, 0, true),
      MenuItem(ActionId::ToggleEditorSurround, "Surround Selection", {}, {}, 0, true),
      MenuItem(ActionId::ToggleEditorSmartIndent, "Smart Indent", {}, {}, 0, true),
      MenuItem(ActionId::ToggleEditorAutoDetectIndent, "Auto-Detect Indent on Open", {}, {}, 0,
               true),
      MenuSeparator(),
      MenuItem(ActionId::ToggleEditorToggleComment, "Toggle Comment Action", {}, {}, 0, true),
      MenuItem(ActionId::ToggleEditorLineOps, "Line Move/Duplicate/Delete", {}, {}, 0, true),
      MenuItem(ActionId::ToggleEditorSortLines, "Sort Lines Action", {}, {}, 0, true),
      MenuItem(ActionId::ToggleEditorSnippets, "Snippets", {}, {}, 0, true),
      MenuSeparator(),
      MenuItem(ActionId::ToggleEditorSaveTrim, "Trim Trailing Whitespace on Save", {}, {}, 0,
               true),
      MenuItem(ActionId::ToggleEditorSaveEnsureNewline, "Ensure Final Newline on Save", {}, {}, 0,
               true),
      MenuSeparator(),
      MenuItem(ActionId::UiScale, "Zoom In", "Ctrl+=", std::array<std::string_view, 2>{"up", {}}, 1),
      MenuItem(ActionId::UiScale, "Zoom Out", "Ctrl+-", std::array<std::string_view, 2>{"down", {}}, 1),
      MenuItem(ActionId::UiScale, "Reset Zoom", "Ctrl+0",
               std::array<std::string_view, 2>{"reset", {}}, 1),
      MenuSeparator(),
      MenuItem(ActionId::PluginsReload, "Reload Plugins"),
  });
  static const auto kHelpItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::OpenKeyboardShortcuts, "Keyboard Shortcuts"),
      MenuItem(ActionId::OpenHelpAbout, "About microide"),
  });
  static const auto kSearchItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::Search),
      MenuItem(ActionId::ReplaceInBuffer),
      MenuItem(ActionId::Files),
      MenuItem(ActionId::ProjectSearch),
  });
  static const auto kTerminalContextItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::CopySelection),
      MenuItem(ActionId::PasteClipboard),
  });
  static const auto kEditorTabContextItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::CloseActiveTab, "Close Tab"),
      MenuItem(ActionId::CloseOtherTabs),
      MenuItem(ActionId::CloseTabsToRight),
      MenuItem(ActionId::CloseTabsToLeft),
      MenuSeparator(),
      MenuItem(ActionId::CopyRelativePath),
      MenuItem(ActionId::CopyAbsolutePath),
  });
  static const auto kTerminalTabContextItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::CopyLastTerminalCommand),
  });
  static const auto kMenus = std::to_array<MenuSpec>({
      MenuSpec{MenuId::File, "File", kFileItems},
      MenuSpec{MenuId::Edit, "Edit", kEditItems},
      MenuSpec{MenuId::Selection, "Selection", kSelectionItems},
      MenuSpec{MenuId::View, "View", kViewItems},
      MenuSpec{MenuId::Go, "Go", kGoItems},
      MenuSpec{MenuId::Git, "Git", kGitItems},
      MenuSpec{MenuId::SidebarMode, "Sidebar Mode", {}},
      MenuSpec{MenuId::GitOutgoingBase, "Outgoing Base", {}},
      MenuSpec{MenuId::Search, "Search", kSearchItems},
      MenuSpec{MenuId::Terminal, "Terminal", kTerminalItems},
      MenuSpec{MenuId::Preferences, "Preferences", kPreferencesItems},
      MenuSpec{MenuId::Help, "Help", kHelpItems},
      MenuSpec{MenuId::EditorContext, "Editor", kEditorContextItems},
      MenuSpec{MenuId::EditorTabContext, "Tabs", kEditorTabContextItems},
      MenuSpec{MenuId::TerminalContext, "Terminal", kTerminalContextItems},
      MenuSpec{MenuId::TerminalTabContext, "Terminal", kTerminalTabContextItems},
  });
  return kMenus;
}

const MenuSpec* FindWorkspaceMenuSpec(MenuId id) {
  const auto menus = WorkspaceMenuSpecs();
  const auto it =
      std::find_if(menus.begin(), menus.end(), [id](const MenuSpec& spec) { return spec.id == id; });
  return it == menus.end() ? nullptr : &(*it);
}

std::span<const MenuItemSpec> WorkspaceTreeContextMenuItems(TreeContextTargetKind target) {
  static const auto kFileItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::OpenSelectedTreeItem),
      MenuItem(ActionId::OpenSelectedTreeItemInNewTab),
      MenuSeparator(),
      MenuItem(ActionId::CompareHead),
      MenuItem(ActionId::Compare),
      MenuSeparator(),
      MenuItem(ActionId::RenamePath),
      MenuItem(ActionId::DeletePath),
      MenuSeparator(),
      MenuItem(ActionId::CopyRelativePath),
      MenuItem(ActionId::CopyAbsolutePath),
  });
  static const auto kDirectoryItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::CreateFile),
      MenuItem(ActionId::CreateDirectory),
      MenuSeparator(),
      MenuItem(ActionId::RenamePath),
      MenuItem(ActionId::DeletePath),
      MenuSeparator(),
      MenuItem(ActionId::TreeRefresh, "Refresh"),
      MenuSeparator(),
      MenuItem(ActionId::CopyRelativePath),
      MenuItem(ActionId::CopyAbsolutePath),
  });
  static const auto kRootItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::CreateFile),
      MenuItem(ActionId::CreateDirectory),
      MenuSeparator(),
      MenuItem(ActionId::TreeRefresh, "Refresh"),
      MenuItem(ActionId::ProjectClose),
      MenuSeparator(),
      MenuItem(ActionId::CopyAbsolutePath),
  });
  static const auto kBackgroundItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::CreateFile),
      MenuItem(ActionId::CreateDirectory),
      MenuSeparator(),
      MenuItem(ActionId::TreeRefresh, "Refresh"),
  });

  switch (target) {
    case TreeContextTargetKind::File:
      return kFileItems;
    case TreeContextTargetKind::Directory:
      return kDirectoryItems;
    case TreeContextTargetKind::Root:
      return kRootItems;
    case TreeContextTargetKind::Background:
      return kBackgroundItems;
    case TreeContextTargetKind::None:
    default:
      return {};
  }
}

MenuId ParseMenuId(std::string_view name) {
  if (name == "file") return MenuId::File;
  if (name == "edit") return MenuId::Edit;
  if (name == "view") return MenuId::View;
  if (name == "search") return MenuId::Search;
  if (name == "terminal") return MenuId::Terminal;
  return MenuId::None;
}

std::vector<ContributedMenuItemView> ContributedMenuItems(
    MenuId menu_id,
    const plugin::PluginHost& plugin_host) {
  std::vector<ContributedMenuItemView> result;
  for (const auto& entry : plugin_host.ContributedMenuEntries()) {
    if (ParseMenuId(entry.menu) != menu_id) {
      continue;
    }
    result.push_back(ContributedMenuItemView{
        .id = entry.id,
        .action = entry.action,
        .label = entry.label,
        .accelerator = entry.accelerator,
        .separator_before = entry.separator_before,
        .plugin_id = entry.plugin_id,
    });
  }
  return result;
}

}  // namespace microide::workspace
