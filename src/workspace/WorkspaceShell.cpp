#include "workspace/WorkspaceShell.h"

#include <array>
#include <string_view>

#include "workspace/WorkspaceCommandRegistry.h"
#include "workspace/WorkspaceShellBootstrapper.h"
#include "workspace/WorkspaceSidebarRegistry.h"

namespace microide::workspace {

std::span<const WorkspaceShell::ActionSpec> WorkspaceShell::ActionSpecs() {
  return WorkspaceCommandSpecs();
}

WorkspaceShell::SidebarMode WorkspaceShell::SidebarModeForViewId(std::string_view view_id) const {
  if (view_id.empty()) {
    return SidebarMode::None;
  }
  const auto view = FindSidebarView(view_id, plugin_runtime_.Host());
  return view.has_value() ? view->mode : SidebarMode::None;
}

WorkspaceShell::SidebarMode WorkspaceShell::ActiveSidebarMode() const {
  return SidebarModeForViewId(context_.current_project_state.sidebar.view_id);
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

ActionAvailability WorkspaceShell::MakeActionAvailability() const {
  return Bootstrapper(*const_cast<WorkspaceShell*>(this)).BuildActionAvailability();
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
  if (ActiveSidebarMode() != SidebarMode::Tree) {
    return nullptr;
  }
  const auto& entries = context_.current_project_state.directory_tree.entries();
  if (context_.current_project_state.directory_tree.selected_index() >= entries.size()) {
    return nullptr;
  }
  return &entries[context_.current_project_state.directory_tree.selected_index()];
}

WorkspaceShell::TreeContextTargetKind WorkspaceShell::SelectedTreeTargetKind() const {
  const project::TreeEntry* entry = SelectedTreeEntry();
  if (entry == nullptr) {
    return TreeContextTargetKind::None;
  }
  if (!entry->is_directory) {
    return TreeContextTargetKind::File;
  }
  return entry->path == context_.current_project_state.root ? TreeContextTargetKind::Root
                                      : TreeContextTargetKind::Directory;
}

}  // namespace microide::workspace
