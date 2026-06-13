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

bool MenuContainsLabel(MenuId id, std::string_view label) {
  const auto* spec = FindWorkspaceMenuSpec(id);
  if (spec == nullptr) {
    return false;
  }
  return std::any_of(spec->items.begin(), spec->items.end(),
                     [label](const MenuItemSpec& item) { return item.label == label; });
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
  Expect(MenuContainsLabel(MenuId::File, "Open Folder / Project Tab…"),
         "File menu should expose Open Folder / Project Tab");
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

}  // namespace

void RegisterWorkspaceMenuRegistryTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceMenuRegistry/TopLevelSnapshot",
          TestMenuRegistryTopLevelSnapshot);
  AddTest(tests, "WorkspaceMenuRegistry/ExpandedMenusExposeExpectedEntries",
          TestMenuRegistryExpandedMenusExposeExpectedEntries);
  AddTest(tests, "WorkspaceMenuRegistry/EveryItemIsWired",
          TestMenuRegistryEveryItemIsWired);
}

}  // namespace microide::tests
