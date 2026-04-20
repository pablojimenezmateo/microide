#include "workspace/WorkspaceShell.h"

#include <array>
#include <string_view>

#include "workspace/WorkspaceCommandRegistry.h"
#include "workspace/WorkspaceSidebarRegistry.h"

namespace microide::workspace {

std::span<const WorkspaceShell::ActionSpec> WorkspaceShell::ActionSpecs() {
  return WorkspaceCommandSpecs();
}

const WorkspaceShell::ActionSpec* WorkspaceShell::FindActionSpec(ActionId id) {
  return FindWorkspaceActionSpec(id);
}

const WorkspaceShell::ActionSpec* WorkspaceShell::FindActionByCommand(std::string_view command_name) {
  return FindWorkspaceActionByCommand(command_name);
}

const std::vector<std::string>& WorkspaceShell::CommandNames() {
  return WorkspaceCommandNames();
}

std::vector<std::string> WorkspaceShell::DocumentedCommandUsages() {
  return WorkspaceDocumentedCommandUsages();
}

bool WorkspaceShell::IsActionEnabled(ActionId id) const {
  switch (id) {
    case ActionId::Colorscheme:
    case ActionId::Files:
    case ActionId::OpenCommandPrompt:
    case ActionId::PluginsReload:
    case ActionId::ProjectOpen:
    case ActionId::Quit:
    case ActionId::SidebarClose:
    case ActionId::SidebarHide:
    case ActionId::SidebarShow:
    case ActionId::SidebarToggle:
      return true;
    case ActionId::CloseActiveTab:
      return !open_tabs_.empty();
    case ActionId::CloseAllTabs:
      return !open_tabs_.empty();
    case ActionId::CloseOtherTabs:
      return open_tabs_.size() > 1;
    case ActionId::CloseTabsToRight:
      return !open_tabs_.empty() && active_tab_index_ + 1 < open_tabs_.size();
    case ActionId::CloseTabsToLeft:
      return !open_tabs_.empty() && active_tab_index_ > 0;
    case ActionId::CompareHead:
    case ActionId::OpenSelectedTreeItem:
    case ActionId::OpenSelectedTreeItemInNewTab:
      return !project_root_.empty() &&
             (menu_state_.tree_context_menu.open ? menu_state_.tree_context_menu.target
                                                 : SelectedTreeTargetKind()) ==
                 TreeContextTargetKind::File;
    case ActionId::CreateDirectory:
    case ActionId::CreateFile: {
      if (project_root_.empty()) {
        return false;
      }
      const TreeContextTargetKind target =
          menu_state_.tree_context_menu.open ? menu_state_.tree_context_menu.target
                                             : SelectedTreeTargetKind();
      return target == TreeContextTargetKind::Directory || target == TreeContextTargetKind::Root ||
             target == TreeContextTargetKind::Background;
    }
    case ActionId::DeletePath:
    case ActionId::RenamePath: {
      if (project_root_.empty()) {
        return false;
      }
      const TreeContextTargetKind target =
          menu_state_.tree_context_menu.open ? menu_state_.tree_context_menu.target
                                             : SelectedTreeTargetKind();
      return target == TreeContextTargetKind::File || target == TreeContextTargetKind::Directory;
    }
    case ActionId::Compare:
    case ActionId::Find:
    case ActionId::GitRefresh:
    case ActionId::Merge:
    case ActionId::Open:
    case ActionId::ProjectClose:
    case ActionId::ProjectSearch:
    case ActionId::Tab:
    case ActionId::Term:
    case ActionId::Tree:
    case ActionId::TreeRefresh:
      return !project_root_.empty();
    case ActionId::CopyLastTerminalCommand:
      return ActiveTerminalTab() != nullptr && LastTerminalCommandText().has_value();
    case ActionId::CopySelectionWithContext:
      return ActiveEditableViewport() != nullptr && ActiveEditableViewport()->has_selection();
    case ActionId::CopySelection:
      return (ActiveEditableViewport() != nullptr && ActiveEditableViewport()->has_selection()) ||
             (surface_.focus == FocusTarget::Panel && TerminalHasSelection());
    case ActionId::CutSelection:
    case ActionId::Redo:
    case ActionId::SelectAll:
    case ActionId::Undo:
      return ActiveEditableViewport() != nullptr;
    case ActionId::PasteClipboard:
      return ActiveEditableViewport() != nullptr ||
             (surface_.focus == FocusTarget::Panel && ActiveTerminalTab() != nullptr);
    case ActionId::Goto:
    case ActionId::Jump:
    case ActionId::ReplaceInBuffer:
    case ActionId::Reopen:
    case ActionId::Search:
    case ActionId::SplitFirst:
    case ActionId::SplitLast:
    case ActionId::SplitNext:
    case ActionId::SplitPrev:
    case ActionId::Unsplit:
    case ActionId::Vsplit:
      return ActiveTabIsEditor();
    case ActionId::Save:
      return ActiveTabIsEditor() || ActiveTabIsMerge() ||
             (ActiveTabIsCompare() && ActiveCompareTab() != nullptr && ActiveCompareTab()->right_editable);
    case ActionId::Focus:
      return true;
    case ActionId::IndentWidth:
      return true;
    case ActionId::CopyAbsolutePath:
      return !ResolveTreeActionPath(ActionSource::ContextMenu).empty();
    case ActionId::CopyRelativePath: {
      const std::filesystem::path path = ResolveTreeActionPath(ActionSource::ContextMenu);
      return !project_root_.empty() && !path.empty() && path != project_root_;
    }
    case ActionId::SidebarWidth:
    case ActionId::SoftTabs:
    case ActionId::TabSize:
    case ActionId::UiScale:
      return true;
    case ActionId::ProjectNext:
    case ActionId::ProjectPrev:
      return !project_root_.empty() && project_catalog_.entries.size() > 1;
    case ActionId::TabMove:
    case ActionId::TabSwitch:
      return !project_root_.empty() && !open_tabs_.empty();
  }

  return true;
}

std::span<const WorkspaceShell::MenuSpec> WorkspaceShell::MenuSpecs() {
  return WorkspaceMenuSpecs();
}

const WorkspaceShell::MenuSpec* WorkspaceShell::FindMenuSpec(MenuId id) {
  return FindWorkspaceMenuSpec(id);
}

std::span<const WorkspaceShell::MenuItemSpec> WorkspaceShell::MenuItems(MenuId id) const {
  if (id != MenuId::SidebarMode) {
    const MenuSpec* menu = FindWorkspaceMenuSpec(id);
    return menu == nullptr ? std::span<const MenuItemSpec>{} : menu->items;
  }

  const auto views = SidebarViews(plugin_runtime_.Host());
  sidebar_mode_menu_items_.clear();
  sidebar_mode_menu_entries_.clear();
  sidebar_mode_menu_items_.reserve(views.size());
  sidebar_mode_menu_entries_.reserve(views.size());

  for (const SidebarViewInfo& view : views) {
    sidebar_mode_menu_entries_.push_back(
        SidebarModeMenuEntry{.label = std::string(view.label), .id = std::string(view.id)});
    const auto& entry = sidebar_mode_menu_entries_.back();
    sidebar_mode_menu_items_.push_back(MenuItemSpec{
        .action = ActionId::SidebarShow,
        .label = entry.label,
        .accelerator = {},
        .args = std::array<std::string_view, 2>{entry.id, {}},
        .arg_count = 1,
        .separator = false,
        .checkable = true,
        .submenu = MenuId::None,
    });
  }

  return sidebar_mode_menu_items_;
}

const project::TreeEntry* WorkspaceShell::SelectedTreeEntry() const {
  if (sidebar_state_.mode != SidebarMode::Tree) {
    return nullptr;
  }
  const auto& entries = directory_tree_.entries();
  if (directory_tree_.selected_index() >= entries.size()) {
    return nullptr;
  }
  return &entries[directory_tree_.selected_index()];
}

WorkspaceShell::TreeContextTargetKind WorkspaceShell::SelectedTreeTargetKind() const {
  const project::TreeEntry* entry = SelectedTreeEntry();
  if (entry == nullptr) {
    return TreeContextTargetKind::None;
  }
  if (!entry->is_directory) {
    return TreeContextTargetKind::File;
  }
  return entry->path == project_root_ ? TreeContextTargetKind::Root
                                      : TreeContextTargetKind::Directory;
}

}  // namespace microide::workspace
