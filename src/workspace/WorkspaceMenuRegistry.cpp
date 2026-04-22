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

}  // namespace

std::span<const MenuSpec> WorkspaceMenuSpecs() {
  static const auto kFileItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::ProjectOpen, "New Project Tab..."),
      MenuSeparator(),
      MenuItem(ActionId::Tab),
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
      MenuItem(ActionId::Completion),
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
      MenuItem(ActionId::CodeActions),
      MenuItem(ActionId::GoToDefinition),
      MenuItem(ActionId::FindReferences),
  });
  static const auto kViewItems = std::to_array<MenuItemSpec>({
      MenuItem(ActionId::SidebarToggle, {}, {}, {}, 0, true),
      MenuSeparator(),
      MenuItem(ActionId::SidebarShow, "Source Control", {},
               std::array<std::string_view, 2>{"git", {}}, 1, true),
      MenuItem(ActionId::SidebarShow, "Problems", {},
               std::array<std::string_view, 2>{"problems", {}}, 1, true),
      MenuItem(ActionId::SidebarShow, "Tests", {},
               std::array<std::string_view, 2>{"tests", {}}, 1, true),
      MenuSeparator(),
      MenuItem(ActionId::ShowOutput),
      MenuItem(ActionId::ShowChat),
      MenuSeparator(),
      MenuItem(ActionId::UiScale, "Zoom In", "Ctrl+=", std::array<std::string_view, 2>{"up", {}},
               1),
      MenuItem(ActionId::UiScale, "Zoom Out", "Ctrl+-",
               std::array<std::string_view, 2>{"down", {}}, 1),
      MenuItem(ActionId::UiScale, "Reset Zoom", "Ctrl+0",
               std::array<std::string_view, 2>{"reset", {}}, 1),
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
      MenuSpec{MenuId::View, "View", kViewItems},
      MenuSpec{MenuId::SidebarMode, "Sidebar Mode", {}},
      MenuSpec{MenuId::Search, "Search", kSearchItems},
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
