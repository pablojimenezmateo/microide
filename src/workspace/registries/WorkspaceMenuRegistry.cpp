#include "workspace/registries/WorkspaceMenuRegistry.h"

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

}  // namespace

bool IsLspDrivenMenuAction(ActionId id) {
  return id == ActionId::GoToDefinition || id == ActionId::GoToTypeDefinition ||
         id == ActionId::GoToImplementation || id == ActionId::GoToDeclaration ||
         id == ActionId::FindReferences || id == ActionId::CallHierarchy ||
         id == ActionId::RenameSymbol || id == ActionId::FormatDocument;
}

std::string_view LspMenuActionFeatureId(ActionId id) {
  switch (id) {
    case ActionId::GoToDefinition:
      return "lsp.goto_definition.enabled";
    case ActionId::GoToTypeDefinition:
    case ActionId::GoToImplementation:
    case ActionId::GoToDeclaration:
      return "lsp.navigation.enabled";
    case ActionId::FindReferences:
      return "lsp.find_references.enabled";
    case ActionId::CallHierarchy:
      return "lsp.call_hierarchy.enabled";
    case ActionId::RenameSymbol:
      return "lsp.rename.enabled";
    case ActionId::FormatDocument:
      return "lsp.formatting.enabled";
    case ActionId::CodeActions:
      return "lsp.code_actions.enabled";
    case ActionId::Completion:
      return "lsp.completion.enabled";
    default:
      return {};
  }
}

bool IsLspMenuActionReady(const LspClient::ReadinessSnapshot& snapshot) {
  return snapshot.state == LspClient::ReadinessSnapshot::State::Ready;
}

std::string_view LspDrivenMenuActionLabel(ActionId id,
                                          std::string_view ready_label,
                                          const LspClient::ReadinessSnapshot& snapshot,
                                          std::string& scratch) {
  if (!IsLspDrivenMenuAction(id) || IsLspMenuActionReady(snapshot)) {
    return ready_label;
  }
  // "Go to Definition (LSP: Starting…)" — the same word the status bar shows, so the
  // greyed-out entry and the bar never disagree about why the action is unavailable.
  std::string word_scratch;
  const std::string_view word = LspReadinessText(snapshot, word_scratch);
  scratch.assign(ready_label);
  scratch += " (LSP: ";
  scratch += word;
  scratch += ")";
  return scratch;
}

std::span<const MenuSpec> WorkspaceMenuSpecs() {
  static const auto kFileItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::Tab, {}, "Ctrl+N"),
      MenuItem(ActionId::Open, {}, "Ctrl+O"),
      MenuItem(ActionId::ProjectOpen, {}, "Ctrl+K Ctrl+O"),
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
      MenuItem(ActionId::AddCursorAtNextMatch, {}, "Ctrl+D"),
      MenuItem(ActionId::AddCursorAtAllMatches, {}, "Ctrl+Shift+L"),
      MenuSeparator(),
      MenuItem(ActionId::ToggleLineComment, {}, "Ctrl+/"),
      MenuItem(ActionId::ToggleBlockComment, {}, "Shift+Alt+A"),
      MenuItem(ActionId::JumpToMatchingBracket, {}, "Ctrl+Shift+\\"),
      MenuSeparator(),
      MenuItem(ActionId::MoveLineUp, {}, "Alt+Up"),
      MenuItem(ActionId::MoveLineDown, {}, "Alt+Down"),
      MenuItem(ActionId::DuplicateLine, {}, "Shift+Alt+Down"),
      MenuItem(ActionId::CopyLineUp, {}, "Shift+Alt+Up"),
      MenuItem(ActionId::InsertLineBelow, {}, "Ctrl+Enter"),
      MenuItem(ActionId::InsertLineAbove, {}, "Ctrl+Shift+Enter"),
      MenuItem(ActionId::DeleteLine, {}, "Ctrl+Shift+K"),
      MenuItem(ActionId::IndentLines, {}, "Tab"),
      MenuItem(ActionId::OutdentLines, {}, "Shift+Tab"),
      MenuSeparator(),
      MenuItem(ActionId::SortLinesAscending),
      MenuItem(ActionId::SortLinesDescending),
      MenuSeparator(),
      MenuItem(ActionId::Fold, {}, "Ctrl+Shift+["),
      MenuItem(ActionId::Unfold, {}, "Ctrl+Shift+]"),
      MenuItem(ActionId::ToggleFoldAtCursor),
      MenuItem(ActionId::FoldAll, {}, "Ctrl+K Ctrl+0"),
      MenuItem(ActionId::UnfoldAll, {}, "Ctrl+K Ctrl+J"),
      MenuSeparator(),
      MenuItem(ActionId::Completion),
      MenuItem(ActionId::InsertSnippet),
      MenuItem(ActionId::CodeActions),
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
      MenuItem(ActionId::RenameSymbol),
      MenuItem(ActionId::FormatDocument),
  });
  static const auto kViewItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::SidebarToggle, {}, {}, {}, 0, true),
      MenuSeparator(),
      MenuItem(ActionId::Wrap, {}, "Alt+Z", {}, 0, true),
      MenuSeparator(),
      MenuItem(ActionId::UiScale, "Zoom In", "Ctrl+=", std::array<std::string_view, 2>{"up", {}},
               1),
      MenuItem(ActionId::UiScale, "Zoom Out", "Ctrl+-",
               std::array<std::string_view, 2>{"down", {}}, 1),
      MenuItem(ActionId::UiScale, "Reset Zoom", "Ctrl+0",
               std::array<std::string_view, 2>{"reset", {}}, 1),
      MenuSeparator(),
      MenuItem(ActionId::ToggleColorTheme),
      MenuItem(ActionId::ToggleFullscreen),
  });
  static const auto kGoItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::OpenCommandPalette, {}, "Ctrl+Shift+P"),
      MenuSeparator(),
      MenuItem(ActionId::Search, {}, "Ctrl+F"),
      MenuItem(ActionId::ReplaceInBuffer, {}, "Ctrl+H"),
      MenuSeparator(),
      MenuItem(ActionId::Files),
      MenuItem(ActionId::ProjectSearch),
      MenuSeparator(),
      MenuItem(ActionId::Goto),
      MenuItem(ActionId::Jump),
      MenuSeparator(),
      MenuItem(ActionId::GoToDefinition),
      MenuItem(ActionId::GoToTypeDefinition),
      MenuItem(ActionId::GoToImplementation),
      MenuItem(ActionId::GoToDeclaration),
      MenuItem(ActionId::FindReferences),
      MenuItem(ActionId::CallHierarchy),
      MenuSeparator(),
      MenuItem(ActionId::GoToNextDiagnostic),
      MenuItem(ActionId::GoToPreviousDiagnostic),
  });
  static const auto kGitItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::SidebarShow, "Source Control Sidebar", "Ctrl+Shift+G",
               std::array<std::string_view, 2>{"git", {}}, 1, false),
      MenuItem(ActionId::GitRefresh, "Refresh"),
      MenuSeparator(),
      MenuItem(ActionId::GitSwitchBranch, "Switch Branch…"),
      MenuSeparator(),
      // Sync first: it is the everyday verb, and grouping pull/push under it keeps
      // the individual halves available without making them the default choice.
      MenuItem(ActionId::GitSync, "Sync"),
      MenuItem(ActionId::GitFetch, "Fetch"),
      MenuItem(ActionId::GitPull, "Pull"),
      MenuItem(ActionId::GitPush, "Push"),
      MenuItem(ActionId::GitPublishBranch, "Publish Branch"),
      MenuSeparator(),
      MenuItem(ActionId::GitStash, "Stash Changes"),
      MenuItem(ActionId::GitStashPop, "Pop Stash"),
      MenuSeparator(),
      MenuItem(ActionId::CompareHead),
  });
  static const auto kTerminalItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::Term),
      MenuItem(ActionId::CopyLastTerminalCommand),
      MenuItem(ActionId::TermClose),
  });
  static const auto kDebugItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::DebugToggleEnabled, {}, {}, {}, 0, true),
      MenuSeparator(),
      MenuItem(ActionId::StartDebugging),
      MenuItem(ActionId::PickLaunchConfig),
      MenuItem(ActionId::StopDebugging),
      MenuItem(ActionId::DebugStopAllSessions, "Stop All Sessions"),
      MenuSeparator(),
      MenuItem(ActionId::BreakpointToggle),
      MenuItem(ActionId::DebugContinue, {}, "F5"),
      MenuItem(ActionId::DebugStepOver, {}, "F10"),
      MenuItem(ActionId::DebugStepIn, {}, "F11"),
      MenuItem(ActionId::DebugStepOut, {}, "Shift+F11"),
      MenuItem(ActionId::DebugPause),
      MenuItem(ActionId::DebugReverseContinue),
      MenuItem(ActionId::DebugStepBack),
      MenuItem(ActionId::DebugRestart, "Restart", "Ctrl+Shift+F5"),
      MenuItem(ActionId::DebugConsoleRepl),
      MenuSeparator(),
      MenuItem(ActionId::DebugPaneToggle, "Show Debug Pane", "Ctrl+Shift+D", {}, 0, true),
      MenuItem(ActionId::DebugPaneShowCallStack, "Call Stack", "Ctrl+Shift+1"),
      MenuItem(ActionId::DebugPaneShowVariables, "Variables", "Ctrl+Shift+2"),
      MenuItem(ActionId::DebugPaneShowWatch, "Watch", "Ctrl+Shift+3"),
      MenuItem(ActionId::DebugPaneShowBreakpoints, "Breakpoints", "Ctrl+Shift+4"),
      MenuItem(ActionId::DebugShowOutput, "Show Output", "Ctrl+Shift+5"),
  });
  static const auto kHelpItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::OpenSettings, {}, "Ctrl+,"),
      MenuItem(ActionId::PluginsReload),
      MenuSeparator(),
      MenuItem(ActionId::OpenHelpAbout),
  });
  static const auto kTerminalContextItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::CopySelection),
      MenuItem(ActionId::PasteClipboard),
  });
  static const auto kEditorTabContextItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::CopyRelativePath),
      MenuItem(ActionId::CopyAbsolutePath),
      MenuItem(ActionId::RevealInFileTree),
      MenuSeparator(),
      MenuItem(ActionId::SelectForCompare),
      MenuItem(ActionId::CompareWithSelected),
      MenuItem(ActionId::CompareWithClipboard),
      MenuSeparator(),
      MenuItem(ActionId::SplitEditorRight, "Split Right", "Ctrl+\\"),
      MenuItem(ActionId::SplitEditorDown, "Split Down"),
      MenuSeparator(),
      MenuItem(ActionId::CloseActiveTab),
      MenuItem(ActionId::CloseAllTabs),
      MenuItem(ActionId::CloseOtherTabs),
      MenuItem(ActionId::CloseTabsToRight),
      MenuItem(ActionId::CloseTabsToLeft),
  });
  static const auto kTerminalTabContextItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::CopyLastTerminalCommand),
      MenuSeparator(),
      MenuItem(ActionId::TermClose),
  });
  static const auto kProjectTabContextItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::ProjectCopyAbsolutePath),
      MenuSeparator(),
      MenuItem(ActionId::ProjectClose),
  });
  static const auto kMenus = std::to_array<MenuSpec>({
      MenuSpec{MenuId::File, "File", kFileItems},
      MenuSpec{MenuId::Edit, "Edit", kEditItems},
      MenuSpec{MenuId::View, "View", kViewItems},
      MenuSpec{MenuId::Go, "Go", kGoItems},
      MenuSpec{MenuId::Git, "Git", kGitItems},
      MenuSpec{MenuId::SidebarMode, "Sidebar Mode", {}},
      MenuSpec{MenuId::GitOutgoingBase, "Outgoing Base", {}},
      MenuSpec{MenuId::Terminal, "Terminal", kTerminalItems},
      MenuSpec{MenuId::Debug, "Debug", kDebugItems},
      MenuSpec{MenuId::Help, "Help", kHelpItems},
      MenuSpec{MenuId::EditorContext, "Editor", kEditorContextItems},
      MenuSpec{MenuId::EditorTabContext, "Tabs", kEditorTabContextItems},
      MenuSpec{MenuId::TerminalContext, "Terminal", kTerminalContextItems},
      MenuSpec{MenuId::TerminalTabContext, "Terminal", kTerminalTabContextItems},
      MenuSpec{MenuId::ProjectTabContext, "Project", kProjectTabContextItems},
  });
  // The cap the menu-bar layout's inline storage is sized from. Adding a menu
  // past this without raising the constant would silently drop it from the bar
  // in a release build (InlineVector asserts in debug and clamps in release), so
  // it is checked here, next to the table, rather than trusted.
  static_assert(std::tuple_size_v<decltype(kMenus)> <= kMaxMenuBarItems,
                "raise kMaxMenuBarItems: the menu-bar layout's inline storage is sized from it");
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
      MenuItem(ActionId::SplitEditorRight, "Split Right", "Ctrl+\\"),
      MenuItem(ActionId::SplitEditorDown, "Split Down"),
      MenuSeparator(),
      MenuItem(ActionId::CompareHead),
      MenuItem(ActionId::Compare),
      MenuItem(ActionId::SelectForCompare),
      MenuItem(ActionId::CompareWithSelected),
      MenuItem(ActionId::CompareWithClipboard),
      MenuSeparator(),
      MenuItem(ActionId::RenamePath),
      MenuItem(ActionId::DeletePath),
      MenuSeparator(),
      MenuItem(ActionId::ShowInFileExplorer),
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
      MenuItem(ActionId::ShowInFileExplorer),
      MenuItem(ActionId::CopyRelativePath),
      MenuItem(ActionId::CopyAbsolutePath),
  });
  static const auto kRootItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::CreateFile),
      MenuItem(ActionId::CreateDirectory),
      MenuSeparator(),
      MenuItem(ActionId::TreeRefresh, "Refresh"),
      MenuSeparator(),
      MenuItem(ActionId::CopyAbsolutePath),
  });
  static const auto kBackgroundItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::CreateFile),
      MenuItem(ActionId::CreateDirectory),
      MenuSeparator(),
      MenuItem(ActionId::TreeRefresh, "Refresh"),
  });
  // Breakpoint gutter context menu (MATLAB-style). The menu only opens on an
  // existing breakpoint; the toggle item's label flips between "Disable" and
  // "Enable" in WorkspaceShell::MenuItemLabel based on the breakpoint's state.
  static const auto kBreakpointItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::DebugBreakpointToggleEnabled, "Disable Breakpoint"),
      MenuItem(ActionId::DebugBreakpointEditCondition, "Set Condition…"),
      // Hit-count breakpoints and logpoints are implemented end-to-end (the
      // Breakpoint fields, the SetBreakpointHitCondition/SetBreakpointLogMessage
      // prompts, EditBreakpointModifierFromMenu's cases, and the DAP resend all
      // exist) but had no menu entry, so neither could be set from the UI.
      MenuItem(ActionId::DebugBreakpointEditHitCondition, "Set Hit Count…"),
      MenuItem(ActionId::DebugBreakpointEditLogMessage, "Set Log Message…"),
      MenuItem(ActionId::DebugBreakpointClearCondition, "Clear Condition"),
      MenuSeparator(),
      MenuItem(ActionId::DebugBreakpointRemove, "Remove Breakpoint"),
  });
  // Git sidebar entry context menu. The Stage/Unstage label flips in
  // WorkspaceShell::MenuItemLabel based on the selected entry's staged flag.
  static const auto kGitEntryItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::GitOpenChanges, "Open Changes"),
      MenuSeparator(),
      MenuItem(ActionId::GitStageToggleEntry, "Stage"),
      MenuItem(ActionId::GitDiscardEntry, "Discard…"),
      MenuSeparator(),
      MenuItem(ActionId::CopyRelativePath),
      MenuItem(ActionId::CopyAbsolutePath),
  });

  // Search hits, problems and tests are all "a row that points at a file". The
  // file tree and git sidebar had context menus from the start; these three
  // swallowed the right button entirely, so the path of a search hit could only
  // be copied by opening the file and using the tab menu.
  static const auto kResultRowItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::RevealInFileTree),
      MenuSeparator(),
      MenuItem(ActionId::ShowInFileExplorer),
      MenuItem(ActionId::CopyRelativePath),
      MenuItem(ActionId::CopyAbsolutePath),
  });

  // Debug pane Variables / Watch rows. The pane was the last interactive list
  // surface in the shell with no right-click menu at all; its Breakpoints rows
  // reuse the gutter's BreakpointLine menu, and its two value surfaces share this
  // one. Both items act on the pane's selected row and disable themselves when
  // that row carries nothing to copy or watch.
  static const auto kDebugValueRowItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::DebugCopyValue, "Copy Value"),
      MenuItem(ActionId::DebugAddToWatch, "Add to Watch"),
  });

  switch (target) {
    case TreeContextTargetKind::File:
      return kFileItems;
    case TreeContextTargetKind::ResultRow:
      return kResultRowItems;
    case TreeContextTargetKind::DebugValueRow:
      return kDebugValueRowItems;
    case TreeContextTargetKind::Directory:
      return kDirectoryItems;
    case TreeContextTargetKind::Root:
      return kRootItems;
    case TreeContextTargetKind::Background:
      return kBackgroundItems;
    case TreeContextTargetKind::BreakpointLine:
      return kBreakpointItems;
    case TreeContextTargetKind::GitEntry:
      return kGitEntryItems;
    case TreeContextTargetKind::None:
    default:
      return {};
  }
}

MenuId ParseMenuId(std::string_view name) {
  if (name == "file") return MenuId::File;
  if (name == "edit") return MenuId::Edit;
  if (name == "view") return MenuId::View;
  if (name == "go") return MenuId::Go;
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
