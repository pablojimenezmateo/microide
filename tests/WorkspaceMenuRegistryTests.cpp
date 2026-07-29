#include "TestSupport.h"

#include "workspace/WorkspaceCommandRegistry.h"
#include "workspace/WorkspaceMenuRegistry.h"

#include <algorithm>
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

// The label the shell paints: a menu item's own override, or the command registry's
// title for its action. Most items carry no override precisely so that the menu and the
// command palette cannot name one command two ways, so asserting on item.label alone
// would only see the handful that do.
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
  const std::array<std::string_view, 7> expected = {
      "File", "Edit", "View", "Go", "Git", "Terminal", "Help",
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
  // The toggle must resolve to a registered command so it is reachable from the
  // palette and dispatches correctly.
  Expect(FindWorkspaceActionSpec(ActionId::DebugToggleEnabled) != nullptr,
         "the Debug enable toggle should resolve to a registered command spec");
}

// Every actionable menu item must resolve to a live command spec. This guards
// against future rewrites leaving a menu entry pointing at a removed ActionId.
void TestMenuRegistryEveryItemIsWired() {
  const auto check_item = [](const MenuItemSpec& item) {
    if (item.separator || item.submenu != MenuId::None) {
      return;
    }
    if (!item.command_name.empty()) {
      return;  // Plugin-contributed / command-name driven items dispatch by name.
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
  // The editor tab strip and project-tree file rows both expose Split Right /
  // Split Down. They route through the same SplitEditorRight/SplitEditorDown
  // actions; availability (greyed once a split exists) is asserted elsewhere.
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
  // Right-clicking an editor tab offers an in-app "Reveal in File Tree" jump to the
  // sidebar tree (distinct from the OS-level "Show in File Explorer").
  Expect(MenuContainsAction(MenuId::EditorTabContext, ActionId::RevealInFileTree),
         "editor tab context menu should expose Reveal in File Tree");
}

void TestMenuRegistryProjectTabContextExposesCopyAbsolutePathNotTreeRoot() {
  // The project tab context menu owns project-level lifecycle: it exposes both
  // Close Project and a dedicated Copy Absolute Path (copies the project root via
  // ExecuteProject, not the active editor file). The project-tree root no longer
  // duplicates Close Project.
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

// Closing a tab/project is destructive and the context menu pre-highlights its
// first enabled item, so copy-path must lead and the close actions must trail.
// This locks in that intent against an accidental future reorder.
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

}  // namespace

void RegisterWorkspaceMenuRegistryTests(std::vector<TestCase>& tests) {
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
}

}  // namespace microide::tests
