#include "TestSupport.h"

#include "workspace/registries/WorkspaceCommandRegistry.h"
#include "workspace/registries/WorkspaceMenuRegistry.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::ActionId;
using microide::workspace::FindWorkspaceActionSpec;
using microide::workspace::FindWorkspaceMenuSpec;
using microide::workspace::MenuId;
using microide::workspace::MenuItemSpec;
using microide::workspace::TreeContextTargetKind;
using microide::workspace::WorkspaceMenuSpecs;
using microide::workspace::WorkspaceTreeContextMenuItems;

std::vector<std::string_view> TopLevelLabels() {
  std::vector<std::string_view> labels;
  for (const auto& spec : WorkspaceMenuSpecs()) {
    switch (spec.id) {
      case MenuId::File:
      case MenuId::Edit:
      case MenuId::View:
      case MenuId::Go:
      case MenuId::Git:
      case MenuId::Remote:
      case MenuId::Terminal:
      case MenuId::Help:
        labels.push_back(spec.label);
        break;
      default:
        break;
    }
  }
  return labels;
}

std::string_view ResolvedMenuItemLabel(const MenuItemSpec& item) {
  if (!item.label.empty()) {
    return item.label;
  }
  const auto* action = FindWorkspaceActionSpec(item.action);
  return action != nullptr ? action->label : std::string_view{};
}

bool MenuContainsLabel(MenuId id, std::string_view label) {
  const auto* spec = FindWorkspaceMenuSpec(id);
  if (spec == nullptr) {
    return false;
  }
  return std::any_of(spec->items.begin(), spec->items.end(), [label](const MenuItemSpec& item) {
    return ResolvedMenuItemLabel(item) == label;
  });
}

bool MenuContainsAction(MenuId id, ActionId action) {
  const auto* spec = FindWorkspaceMenuSpec(id);
  if (spec == nullptr) {
    return false;
  }
  return std::any_of(spec->items.begin(), spec->items.end(),
                     [action](const MenuItemSpec& item) { return item.action == action; });
}

bool TreeMenuContainsAction(TreeContextTargetKind target, ActionId action) {
  const auto items = WorkspaceTreeContextMenuItems(target);
  return std::any_of(items.begin(), items.end(),
                     [action](const MenuItemSpec& item) { return item.action == action; });
}

void TestMenuRegistryTopLevelSnapshot() {
  const std::vector<std::string_view> labels = TopLevelLabels();
  const std::array<std::string_view, 8> expected = {
      "File", "Edit", "View", "Go", "Git", "Remote", "Terminal", "Help",
  };
  Expect(labels.size() == expected.size(),
         "top-level menu snapshot should contain the expected number of menus");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    Expect(i < labels.size() && labels[i] == expected[i],
           "top-level menu snapshot should preserve declared order");
  }
}

void TestMenuRegistryExpandedMenusExposeExpectedEntries() {
  Expect(MenuContainsLabel(MenuId::File, "Open File…"),
         "File menu should expose Open File");
  Expect(MenuContainsLabel(MenuId::File, "Open Folder…"),
         "File menu should expose Open Folder");
  Expect(!MenuContainsAction(MenuId::File, ActionId::RemoteOpenFolder),
         "File menu should not also list Open Folder on Host");
  {
    const auto* terminal = FindWorkspaceMenuSpec(MenuId::Terminal);
    Expect(terminal != nullptr && terminal->items.size() > 1 && terminal->items[0].action == ActionId::Term &&
               terminal->items[1].action == ActionId::RemoteOpenLocalTerminal &&
               ResolvedMenuItemLabel(terminal->items[1]) == "New Local Terminal",
           "Terminal menu should expose New Local Terminal directly under New Terminal");
    Expect(!MenuContainsLabel(MenuId::Terminal, "Remote: Open Terminal on Host…"),
           "the removed host-terminal command is not in the Terminal menu");
  }
  Expect(MenuContainsLabel(MenuId::Go, "Go to File…"),
         "Go menu should expose file navigation");
  Expect(MenuContainsLabel(MenuId::Go, "Find in Buffer"),
         "Go menu should absorb the buffer search command");
  Expect(MenuContainsLabel(MenuId::Go, "Replace in Buffer"),
         "Go menu should absorb the buffer replace command");
  Expect(MenuContainsLabel(MenuId::Edit, "Add Cursor at Next Match"),
         "Edit menu should expose the multicursor command folded in from Selection");
  Expect(MenuContainsLabel(MenuId::Git, "Compare with HEAD"),
         "Git menu should expose compare with HEAD");
  Expect(MenuContainsLabel(MenuId::Terminal, "New Terminal"),
         "Terminal menu should expose terminal creation");
  Expect(MenuContainsLabel(MenuId::Help, "Reload Plugins"),
         "Help menu should expose plugin reload");
  Expect(MenuContainsLabel(MenuId::Help, "Settings…"),
         "Help menu should expose the settings overlay");
  Expect(MenuContainsLabel(MenuId::Help, "Help"),
         "Help menu should expose the combined Help item");
  Expect(!MenuContainsLabel(MenuId::Help, "Keyboard Shortcuts"),
         "Help menu should no longer expose a separate Keyboard Shortcuts item");
  Expect(!MenuContainsLabel(MenuId::Help, "About microide"),
         "Help menu should no longer expose a separate About microide item");
}

void TestMenuRegistryDebugMenuLeadsWithEnableToggle() {
  using microide::workspace::ActionId;
  const auto* spec = FindWorkspaceMenuSpec(MenuId::Debug);
  Expect(spec != nullptr, "Debug menu should exist in the registry");
  if (spec == nullptr || spec->items.empty()) {
    return;
  }
  const MenuItemSpec& first = spec->items.front();
  Expect(first.action == ActionId::DebugToggleEnabled,
         "Debug menu's first item should be the enable/disable toggle");
  Expect(ResolvedMenuItemLabel(first) == "Enable Debugger",
         "Debug toggle should be labelled 'Enable Debugger'");
  Expect(first.checkable, "Debug toggle should be a checkable item");
  Expect(spec->items.size() > 1 && spec->items[1].separator,
         "a separator should follow the Debug enable toggle");
  Expect(FindWorkspaceActionSpec(ActionId::DebugToggleEnabled) != nullptr,
         "the Debug enable toggle should resolve to a registered command spec");
}

void TestMenuRegistryEveryItemIsWired() {
  const auto check_item = [](const MenuItemSpec& item) {
    if (item.separator || item.submenu != MenuId::None) {
      return;
    }
    if (!item.command_name.empty()) {
      return;
    }
    Expect(FindWorkspaceActionSpec(item.action) != nullptr,
           "every menu item action should resolve to a registered command spec");
  };

  for (const auto& spec : WorkspaceMenuSpecs()) {
    for (const auto& item : spec.items) {
      check_item(item);
    }
  }

  const std::array<TreeContextTargetKind, 4> tree_targets = {
      TreeContextTargetKind::File,
      TreeContextTargetKind::Directory,
      TreeContextTargetKind::Root,
      TreeContextTargetKind::Background,
  };
  for (const auto target : tree_targets) {
    for (const auto& item : WorkspaceTreeContextMenuItems(target)) {
      check_item(item);
    }
  }
}

void TestMenuRegistrySplitItemsPresentInTabAndTreeMenus() {
  Expect(MenuContainsAction(MenuId::EditorTabContext, ActionId::SplitEditorRight),
         "editor tab context menu should expose Split Right");
  Expect(MenuContainsAction(MenuId::EditorTabContext, ActionId::SplitEditorDown),
         "editor tab context menu should expose Split Down");
  Expect(TreeMenuContainsAction(TreeContextTargetKind::File, ActionId::SplitEditorRight),
         "project-tree file context menu should expose Split Right");
  Expect(TreeMenuContainsAction(TreeContextTargetKind::File, ActionId::SplitEditorDown),
         "project-tree file context menu should expose Split Down");
}

void TestMenuRegistryEditorTabContextExposesRevealInFileTree() {
  Expect(MenuContainsAction(MenuId::EditorTabContext, ActionId::RevealInFileTree),
         "editor tab context menu should expose Reveal in File Tree");
}

void TestMenuRegistryProjectTabContextExposesCopyAbsolutePathNotTreeRoot() {
  Expect(MenuContainsAction(MenuId::ProjectTabContext, ActionId::ProjectClose),
         "project tab context menu should keep Close Project");
  Expect(MenuContainsAction(MenuId::ProjectTabContext, ActionId::ProjectCopyAbsolutePath),
         "project tab context menu should expose Copy Absolute Path");
  Expect(!TreeMenuContainsAction(TreeContextTargetKind::Root, ActionId::ProjectClose),
         "project-tree root context menu should no longer expose Close Project");
}

const MenuItemSpec* FirstActionableItem(MenuId id) {
  const auto* spec = FindWorkspaceMenuSpec(id);
  if (spec == nullptr) {
    return nullptr;
  }
  for (const auto& item : spec->items) {
    if (!item.separator) {
      return &item;
    }
  }
  return nullptr;
}

const MenuItemSpec* LastActionableItem(MenuId id) {
  const auto* spec = FindWorkspaceMenuSpec(id);
  if (spec == nullptr) {
    return nullptr;
  }
  for (auto it = spec->items.rbegin(); it != spec->items.rend(); ++it) {
    if (!it->separator) {
      return &*it;
    }
  }
  return nullptr;
}

void TestMenuRegistryTabContextMenusLeadWithCopyAndTrailWithClose() {
  const MenuItemSpec* editor_first = FirstActionableItem(MenuId::EditorTabContext);
  const MenuItemSpec* editor_last = LastActionableItem(MenuId::EditorTabContext);
  Expect(editor_first != nullptr && editor_first->action == ActionId::CopyRelativePath,
         "editor tab context menu should lead with Copy Relative Path");
  Expect(editor_last != nullptr && editor_last->action == ActionId::CloseTabsToLeft,
         "editor tab context menu should trail with the close actions");

  const MenuItemSpec* project_first = FirstActionableItem(MenuId::ProjectTabContext);
  const MenuItemSpec* project_last = LastActionableItem(MenuId::ProjectTabContext);
  Expect(project_first != nullptr && project_first->action == ActionId::ProjectCopyAbsolutePath,
         "project tab context menu should lead with Copy Absolute Path");
  Expect(project_last != nullptr && project_last->action == ActionId::ProjectClose,
         "project tab context menu should trail with Close Project");
}

void TestMenuRegistryEveryMenuIdHasASpec() {
  int checked = 0;
  for (int raw = static_cast<int>(MenuId::None) + 1; raw < static_cast<int>(microide::workspace::kMenuIdCount);
       ++raw) {
    const MenuId id = static_cast<MenuId>(raw);
    Expect(FindWorkspaceMenuSpec(id) != nullptr,
           "every MenuId enumerator must have a MenuSpec in the registry table");
    ++checked;
  }
  Expect(checked == static_cast<int>(WorkspaceMenuSpecs().size()),
         "the enum and the spec table must have the same number of menus -- a spec "
         "listed twice would otherwise hide a missing one");
}

void TestMenuRegistryRemoteMenuHoldsTheRemoteVerbs() {
  const auto* spec = FindWorkspaceMenuSpec(MenuId::Remote);
  Expect(spec != nullptr && spec->label == "Remote", "the Remote menu should exist");
  if (spec == nullptr) {
    return;
  }
  struct Row {
    bool separator;
    ActionId action;
    std::string_view label;
  };
  const Row expected[] = {
      {false, ActionId::RemoteOpenFolder, "Open Folder on Host…"},
      {true, {}, {}},
      {false, ActionId::RemoteShowStatus, "Show Status"},
      {false, ActionId::RemoteShowHostChanges, "Show Changed on Host"},
      {false, ActionId::RemoteFileHistory, "File History…"},
      {false, ActionId::RemoteShowConflicts, "Show Conflicts"},
      {false, ActionId::RemoteShowLog, "Show Host Server Log"},
      {true, {}, {}},
      {false, ActionId::RemoteForwardPort, "Forward a Port…"},
      {false, ActionId::RemoteUnforwardPort, "Stop Forwarding a Port…"},
      {true, {}, {}},
      {false, ActionId::RemoteReconnect, "Reconnect"},
      {false, ActionId::RemoteDisconnect, "Disconnect"},
      {false, ActionId::RemoteStopServer, "Stop Host Server"},
      {true, {}, {}},
      {false, ActionId::RemoteRemoveMirror, "Remove Mirror…"},
  };
  Expect(spec->items.size() == std::size(expected),
         "the Remote menu should hold " + std::to_string(std::size(expected)) + " rows, holds " +
             std::to_string(spec->items.size()));
  for (std::size_t i = 0; i < std::min(spec->items.size(), std::size(expected)); ++i) {
    const MenuItemSpec& item = spec->items[i];
    if (expected[i].separator) {
      Expect(item.separator, "Remote menu row " + std::to_string(i) + " should be a separator");
      continue;
    }
    Expect(!item.separator && item.action == expected[i].action &&
               ResolvedMenuItemLabel(item) == expected[i].label,
           "Remote menu row " + std::to_string(i) + " should be \"" + std::string(expected[i].label) +
               "\", is \"" + std::string(ResolvedMenuItemLabel(item)) + "\"");
    const auto* action = FindWorkspaceActionSpec(item.action);
    Expect(!ResolvedMenuItemLabel(item).starts_with("Remote"),
           "a Remote menu label should not repeat the menu's name");
    Expect(action != nullptr && action->label.starts_with("Remote: "),
           "the palette label keeps the Remote: prefix (typing \"remote\" finds it): " +
               std::string(action != nullptr ? action->label : std::string_view("?")));
  }
  const auto labels = TopLevelLabels();
  const auto git = std::find(labels.begin(), labels.end(), "Git");
  Expect(git != labels.end() && git + 1 != labels.end() && *(git + 1) == "Remote" && git + 2 != labels.end() &&
             *(git + 2) == "Terminal",
         "the Remote menu should sit between Git and Terminal");
}

void TestMenuRegistryParseMenuIdNamesEveryBarMenu() {
  using microide::workspace::IsMenuBarTopLevelMenu;
  using microide::workspace::ParseMenuId;
  for (const auto& spec : WorkspaceMenuSpecs()) {
    std::string lower(spec.label);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const MenuId parsed = ParseMenuId(lower);
    if (IsMenuBarTopLevelMenu(spec.id)) {
      Expect(parsed == spec.id, "\"" + lower + "\" should name the " + std::string(spec.label) + " menu");
    } else {
      Expect(parsed == MenuId::None || IsMenuBarTopLevelMenu(parsed),
             "\"" + lower + "\" should not name a context menu");
    }
  }
  Expect(ParseMenuId("remote") == MenuId::Remote, "\"remote\" should name the Remote menu");
  Expect(ParseMenuId("tools") == MenuId::None, "an unknown name should name nothing");
  Expect(ParseMenuId("") == MenuId::None, "an empty name should name nothing");
}

}

void RegisterWorkspaceMenuRegistryTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceMenuRegistry/EveryMenuIdHasASpec",
          TestMenuRegistryEveryMenuIdHasASpec);
  AddTest(tests, "WorkspaceMenuRegistry/TopLevelSnapshot",
          TestMenuRegistryTopLevelSnapshot);
  AddTest(tests, "WorkspaceMenuRegistry/ExpandedMenusExposeExpectedEntries",
          TestMenuRegistryExpandedMenusExposeExpectedEntries);
  AddTest(tests, "WorkspaceMenuRegistry/DebugMenuLeadsWithEnableToggle",
          TestMenuRegistryDebugMenuLeadsWithEnableToggle);
  AddTest(tests, "WorkspaceMenuRegistry/EveryItemIsWired",
          TestMenuRegistryEveryItemIsWired);
  AddTest(tests, "WorkspaceMenuRegistry/SplitItemsPresentInTabAndTreeMenus",
          TestMenuRegistrySplitItemsPresentInTabAndTreeMenus);
  AddTest(tests, "WorkspaceMenuRegistry/EditorTabContextExposesRevealInFileTree",
          TestMenuRegistryEditorTabContextExposesRevealInFileTree);
  AddTest(tests, "WorkspaceMenuRegistry/ProjectTabContextExposesCopyAbsolutePathNotTreeRoot",
          TestMenuRegistryProjectTabContextExposesCopyAbsolutePathNotTreeRoot);
  AddTest(tests, "WorkspaceMenuRegistry/TabContextMenusLeadWithCopyAndTrailWithClose",
          TestMenuRegistryTabContextMenusLeadWithCopyAndTrailWithClose);
  AddTest(tests, "WorkspaceMenuRegistry/RemoteMenuHoldsTheRemoteVerbs", TestMenuRegistryRemoteMenuHoldsTheRemoteVerbs);
  AddTest(tests, "WorkspaceMenuRegistry/ParseMenuIdNamesEveryBarMenu", TestMenuRegistryParseMenuIdNamesEveryBarMenu);
}

}
