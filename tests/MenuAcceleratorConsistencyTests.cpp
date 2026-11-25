#include "TestSupport.h"

#include "plugin/PluginHost.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"
#include "workspace/registries/WorkspaceKeybindingRegistry.h"
#include "workspace/registries/WorkspaceMenuRegistry.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::ActionId;
using microide::workspace::BuiltinKeybindingSpecs;
using microide::workspace::FindWorkspaceActionByCommand;
using microide::workspace::FindWorkspaceActionSpec;
using microide::workspace::FormatBindingKeys;
using microide::workspace::KeybindingSpec;
using microide::workspace::MenuId;
using microide::workspace::MenuItemSpec;
using microide::workspace::MenuSpec;
using microide::workspace::TreeContextTargetKind;
using microide::workspace::WorkspaceMenuSpecs;
using microide::workspace::WorkspaceTreeContextMenuItems;

std::string_view ResolvedAccelerator(const MenuItemSpec& item) {
  if (!item.accelerator.empty()) {
    return item.accelerator;
  }
  if (!item.command_name.empty()) {
    if (const auto* action = FindWorkspaceActionByCommand(item.command_name);
        action != nullptr && !action->accelerator.empty()) {
      return action->accelerator;
    }
  }
  if (const auto* action = FindWorkspaceActionSpec(item.action);
      action != nullptr && !action->accelerator.empty()) {
    return action->accelerator;
  }
  return {};
}

std::string_view ResolvedLabel(const MenuItemSpec& item) {
  if (!item.label.empty()) {
    return item.label;
  }
  if (const auto* action = FindWorkspaceActionSpec(item.action);
      action != nullptr && !action->label.empty()) {
    return action->label;
  }
  return "(unlabelled)";
}

bool IsContextuallyDispatched(std::string_view accelerator) {
  return accelerator == "Tab" || accelerator == "Shift+Tab";
}

bool ArgsMatch(const MenuItemSpec& item, const KeybindingSpec& binding) {
  if (item.arg_count != binding.arg_count) {
    return false;
  }
  for (std::size_t i = 0; i < item.arg_count; ++i) {
    if (item.args[i] != binding.args[i]) {
      return false;
    }
  }
  return true;
}

std::vector<const KeybindingSpec*> BindingsForItem(const MenuItemSpec& item) {
  std::vector<const KeybindingSpec*> matches;
  for (const KeybindingSpec& binding : BuiltinKeybindingSpecs()) {
    if (binding.action == item.action && ArgsMatch(item, binding)) {
      matches.push_back(&binding);
    }
  }
  return matches;
}

void ForEachMenuItem(const std::function<void(std::string_view, const MenuItemSpec&)>& visit) {
  for (const MenuSpec& menu : WorkspaceMenuSpecs()) {
    for (const MenuItemSpec& item : menu.items) {
      if (!item.separator) {
        visit(menu.label.empty() ? std::string_view{"(context)"} : menu.label, item);
      }
    }
  }
  for (const TreeContextTargetKind target :
       {TreeContextTargetKind::File, TreeContextTargetKind::Directory,
        TreeContextTargetKind::Root, TreeContextTargetKind::Background,
        TreeContextTargetKind::BreakpointLine, TreeContextTargetKind::GitEntry}) {
    for (const MenuItemSpec& item : WorkspaceTreeContextMenuItems(target)) {
      if (!item.separator) {
        visit("tree context", item);
      }
    }
  }
}

void TestMenuAcceleratorsMatchTheirKeybindings() {
  std::string mismatches;
  ForEachMenuItem([&](std::string_view menu, const MenuItemSpec& item) {
    const std::string_view accelerator = ResolvedAccelerator(item);
    if (accelerator.empty() || IsContextuallyDispatched(accelerator)) {
      return;
    }
    const std::vector<const KeybindingSpec*> bindings = BindingsForItem(item);
    if (bindings.empty()) {
      mismatches += "\n  ";
      mismatches += menu;
      mismatches += " / ";
      mismatches += ResolvedLabel(item);
      mismatches += ": advertises \"";
      mismatches += accelerator;
      mismatches += "\" but no keybinding produces that action";
      return;
    }
    bool any_match = true;
    for (std::size_t from = 0; from <= accelerator.size();) {
      const std::size_t slash = accelerator.find(" / ", from);
      const std::string_view alternative =
          accelerator.substr(from, slash == std::string_view::npos ? std::string_view::npos : slash - from);
      any_match = any_match && std::any_of(bindings.begin(), bindings.end(), [&](const KeybindingSpec* binding) {
                    return FormatBindingKeys(*binding) == alternative;
                  });
      if (slash == std::string_view::npos) {
        break;
      }
      from = slash + 3;
    }
    if (!any_match) {
      mismatches += "\n  ";
      mismatches += menu;
      mismatches += " / ";
      mismatches += ResolvedLabel(item);
      mismatches += ": advertises \"";
      mismatches += accelerator;
      mismatches += "\" but is bound to \"";
      mismatches += FormatBindingKeys(*bindings.front());
      mismatches += "\"";
    }
  });
  Expect(mismatches.empty(),
         "every menu accelerator must name the key that actually fires its action:" +
             mismatches);
}

void TestBoundMenuActionsAdvertiseTheirAccelerator() {
  std::string missing;
  ForEachMenuItem([&](std::string_view menu, const MenuItemSpec& item) {
    if (!ResolvedAccelerator(item).empty()) {
      return;
    }
    const std::vector<const KeybindingSpec*> bindings = BindingsForItem(item);
    if (bindings.empty()) {
      return;
    }
    missing += "\n  ";
    missing += menu;
    missing += " / ";
    missing += ResolvedLabel(item);
    missing += ": bound to \"";
    missing += FormatBindingKeys(*bindings.front());
    missing += "\" but the menu shows no accelerator";
  });
  Expect(missing.empty(),
         "every menu entry whose action has a keybinding must advertise it:" + missing);
}

}

void RegisterMenuAcceleratorConsistencyTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MenuAccelerators/MatchTheirKeybindings",
          TestMenuAcceleratorsMatchTheirKeybindings);
  AddTest(tests, "MenuAccelerators/BoundActionsAdvertiseTheirAccelerator",
          TestBoundMenuActionsAdvertiseTheirAccelerator);
}

}
