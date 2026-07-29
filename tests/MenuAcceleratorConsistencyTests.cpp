#include "TestSupport.h"

#include "plugin/PluginHost.h"
#include "workspace/WorkspaceCommandRegistry.h"
#include "workspace/WorkspaceKeybindingRegistry.h"
#include "workspace/WorkspaceMenuRegistry.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Menu accelerators are hand-written string literals on each MenuItemSpec, while the
// keys that actually fire live in the keybinding registry. Nothing connected the two,
// so a menu could advertise a chord that was never bound (or that had since moved) and
// the only way to notice was to try it. These tests are that connection: every
// accelerator a menu paints must resolve to a real binding for the same action, and
// every action a menu exposes that *has* a binding must advertise it.

namespace microide::tests {
namespace {

using microide::workspace::ActionId;
using microide::workspace::BuiltinKeybindingSpecs;
using microide::workspace::FindWorkspaceActionByCommand;
using microide::workspace::FindWorkspaceActionSpec;
using microide::workspace::FormatKeyChord;
using microide::workspace::KeybindingSpec;
using microide::workspace::MenuId;
using microide::workspace::MenuItemSpec;
using microide::workspace::MenuSpec;
using microide::workspace::TreeContextTargetKind;
using microide::workspace::WorkspaceMenuSpecs;
using microide::workspace::WorkspaceTreeContextMenuItems;

// The accelerator the shell actually paints, resolving exactly as
// WorkspaceShell::MenuItemAccelerator does: the item's own literal first, then the
// bound command's action spec, then the item action's spec.
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

// A menu item's display label, resolving through the action spec the way the shell does.
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

// Keys the registry deliberately does not own, because what they do depends on
// surrounding editor state rather than on a binding:
//   - Two-key sequences (a Ctrl+K leader plus a follow-up) are dispatched by hand in
//     KeyInputCoordinator::HandleGlobalKeyDown.
//   - Tab / Shift+Tab indent only when the editor is not completing a snippet or a
//     completion, so they live in the editor key path, not the binding table.
// They still have to be spelled correctly, which is what the label check upholds;
// only the "which binding produces this" half is skipped.
bool IsContextuallyDispatched(std::string_view accelerator) {
  return accelerator.find(' ') != std::string_view::npos || accelerator == "Tab" ||
         accelerator == "Shift+Tab";
}

// Same action *and* same arguments: several actions (UiScale up/down/reset,
// DebugPaneShow*) appear more than once with different args and different keys.
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

// Every menu the shell can paint, including the context menus, which are reached
// through their own accessor rather than WorkspaceMenuSpecs().
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
    const bool any_match = std::any_of(
        bindings.begin(), bindings.end(), [&](const KeybindingSpec* binding) {
          return FormatKeyChord(binding->key, binding->modifiers) == accelerator;
        });
    if (!any_match) {
      mismatches += "\n  ";
      mismatches += menu;
      mismatches += " / ";
      mismatches += ResolvedLabel(item);
      mismatches += ": advertises \"";
      mismatches += accelerator;
      mismatches += "\" but is bound to \"";
      mismatches += FormatKeyChord(bindings.front()->key, bindings.front()->modifiers);
      mismatches += "\"";
    }
  });
  Expect(mismatches.empty(),
         "every menu accelerator must name the key that actually fires its action:" +
             mismatches);
}

// The mirror-image check: a menu entry whose action *is* bound must say so, otherwise
// the shortcut exists but is undiscoverable from the menu that offers the command.
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
    missing += FormatKeyChord(bindings.front()->key, bindings.front()->modifiers);
    missing += "\" but the menu shows no accelerator";
  });
  Expect(missing.empty(),
         "every menu entry whose action has a keybinding must advertise it:" + missing);
}

// Built-in bindings are resolved in declaration order and FindKeybinding returns the
// first match, so two built-ins on the same chord with overlapping contexts leave the
// second permanently dead — advertised in Help and Settings, never dispatched. Plugin
// contributions are already screened against the resolved set (ResolveKeybindings
// skips a shadowed chord); nothing screened the built-ins against each other.
void TestBuiltinKeybindingsDoNotShadowEachOther() {
  const auto overlaps = [](microide::workspace::KeybindingContext a,
                           microide::workspace::KeybindingContext b) {
    return a == b || a == microide::workspace::KeybindingContext::Global ||
           b == microide::workspace::KeybindingContext::Global;
  };

  std::string collisions;
  const std::span<const KeybindingSpec> specs = BuiltinKeybindingSpecs();
  for (std::size_t i = 0; i < specs.size(); ++i) {
    for (std::size_t j = i + 1; j < specs.size(); ++j) {
      if (specs[i].key != specs[j].key ||
          microide::workspace::NormalizedKeyModifiers(specs[i].modifiers) !=
              microide::workspace::NormalizedKeyModifiers(specs[j].modifiers) ||
          !overlaps(specs[i].context, specs[j].context)) {
        continue;
      }
      collisions += "\n  ";
      collisions += specs[i].id;
      collisions += " and ";
      collisions += specs[j].id;
      collisions += " both claim ";
      collisions += FormatKeyChord(specs[i].key, specs[i].modifiers);
      collisions += " in overlapping contexts (the second can never fire)";
    }
  }
  Expect(collisions.empty(),
         "no two built-in keybindings may claim the same chord in overlapping contexts:" +
             collisions);
}

}  // namespace

void RegisterMenuAcceleratorConsistencyTests(std::vector<TestCase>& tests) {
  AddTest(tests, "MenuAccelerators/BuiltinKeybindingsDoNotShadowEachOther",
          TestBuiltinKeybindingsDoNotShadowEachOther);
  AddTest(tests, "MenuAccelerators/MatchTheirKeybindings",
          TestMenuAcceleratorsMatchTheirKeybindings);
  AddTest(tests, "MenuAccelerators/BoundActionsAdvertiseTheirAccelerator",
          TestBoundMenuActionsAdvertiseTheirAccelerator);
}

}  // namespace microide::tests
