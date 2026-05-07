#include "TestSupport.h"

#include "workspace/WorkspaceMenuRegistry.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::FindWorkspaceMenuSpec;
using microide::workspace::MenuId;
using microide::workspace::MenuItemSpec;
using microide::workspace::WorkspaceMenuSpecs;

std::vector<std::string_view> TopLevelLabels() {
  std::vector<std::string_view> labels;
  for (const auto& spec : WorkspaceMenuSpecs()) {
    switch (spec.id) {
      case MenuId::File:
      case MenuId::Edit:
      case MenuId::Selection:
      case MenuId::View:
      case MenuId::Go:
      case MenuId::Run:
      case MenuId::Git:
      case MenuId::Search:
      case MenuId::Terminal:
      case MenuId::Preferences:
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
  const std::array<std::string_view, 11> expected = {
      "File", "Edit", "Selection", "View", "Go", "Run",
      "Git", "Search", "Terminal", "Preferences", "Help",
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
  Expect(MenuContainsLabel(MenuId::Run, "Run Tests"),
         "Run menu should expose test execution");
  Expect(MenuContainsLabel(MenuId::Git, "Compare with HEAD"),
         "Git menu should expose compare with HEAD");
  Expect(MenuContainsLabel(MenuId::Terminal, "New Terminal"),
         "Terminal menu should expose terminal creation");
  Expect(MenuContainsLabel(MenuId::Preferences, "Reload Plugins"),
         "Preferences menu should expose plugin reload");
  Expect(MenuContainsLabel(MenuId::Preferences, "Settings…"),
         "Preferences menu should expose the settings overlay");
  Expect(MenuContainsLabel(MenuId::Preferences, "AI Provider…"),
         "Preferences menu should expose the AI provider picker");
  Expect(MenuContainsLabel(MenuId::Help, "Keyboard Shortcuts"),
         "Help menu should expose keyboard shortcuts");
  Expect(MenuContainsLabel(MenuId::Help, "About microide"),
         "Help menu should expose About microide");
}

}  // namespace

void RegisterWorkspaceMenuRegistryTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceMenuRegistry/TopLevelSnapshot",
          TestMenuRegistryTopLevelSnapshot);
  AddTest(tests, "WorkspaceMenuRegistry/ExpandedMenusExposeExpectedEntries",
          TestMenuRegistryExpandedMenusExposeExpectedEntries);
}

}  // namespace microide::tests
