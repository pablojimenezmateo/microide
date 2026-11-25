#include "TestSupport.h"

#include "workspace/registries/WorkspaceCommandRegistry.h"
#include "workspace/registries/WorkspaceMenuRegistry.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::ActionId;
using microide::workspace::ActionSpec;
using microide::workspace::FindWorkspaceActionByCommand;
using microide::workspace::FindWorkspaceActionSpec;
using microide::workspace::MenuId;
using microide::workspace::MenuItemSpec;
using microide::workspace::MenuSpec;
using microide::workspace::TreeContextTargetKind;
using microide::workspace::WorkspaceCommandSpecs;
using microide::workspace::WorkspaceMenuSpecs;
using microide::workspace::WorkspaceTreeContextMenuItems;

constexpr std::string_view kEllipsis = "\xE2\x80\xA6";

const std::array<TreeContextTargetKind, 8> kTreeTargets = {
    TreeContextTargetKind::File,          TreeContextTargetKind::Directory,
    TreeContextTargetKind::Root,          TreeContextTargetKind::Background,
    TreeContextTargetKind::BreakpointLine, TreeContextTargetKind::GitEntry,
    TreeContextTargetKind::ResultRow,     TreeContextTargetKind::DebugValueRow,
};

std::string_view ResolvedLabel(const MenuItemSpec& item) {
  if (!item.label.empty()) {
    return item.label;
  }
  if (!item.command_name.empty()) {
    if (const ActionSpec* action = FindWorkspaceActionByCommand(item.command_name);
        action != nullptr && !action->label.empty()) {
      return action->label;
    }
  }
  const ActionSpec* action = FindWorkspaceActionSpec(item.action);
  return action != nullptr ? action->label : std::string_view{};
}

void ForEachMenuItem(
    const std::function<void(std::string_view, const MenuItemSpec&)>& visit) {
  for (const MenuSpec& menu : WorkspaceMenuSpecs()) {
    for (const MenuItemSpec& item : menu.items) {
      if (!item.separator) {
        visit(menu.label.empty() ? std::string_view{"(context)"} : menu.label, item);
      }
    }
  }
  for (const TreeContextTargetKind target : kTreeTargets) {
    for (const MenuItemSpec& item : WorkspaceTreeContextMenuItems(target)) {
      if (!item.separator) {
        visit("tree context", item);
      }
    }
  }
}

bool EndsWithEllipsis(std::string_view label) { return label.ends_with(kEllipsis); }

void TestNoUserFacingLabelUsesAsciiEllipsis() {
  std::string offenders;
  for (const ActionSpec& spec : WorkspaceCommandSpecs()) {
    if (spec.label.find("...") != std::string_view::npos) {
      offenders += "\n  command title \"";
      offenders += spec.label;
      offenders += "\"";
    }
  }
  ForEachMenuItem([&](std::string_view menu, const MenuItemSpec& item) {
    const std::string_view label = ResolvedLabel(item);
    if (label.find("...") != std::string_view::npos) {
      offenders += "\n  ";
      offenders += menu;
      offenders += " item \"";
      offenders += label;
      offenders += "\"";
    }
  });
  Expect(offenders.empty(),
         std::string("user-facing labels must spell an ellipsis \"…\" (U+2026), not \"...\":") +
             offenders);
}

void TestMenuAndPaletteAgreeOnTheEllipsisPromise() {
  std::string mismatches;
  ForEachMenuItem([&](std::string_view menu, const MenuItemSpec& item) {
    if (item.arg_count > 0 || item.label.empty() || !item.command_name.empty()) {
      return;
    }
    const ActionSpec* action = FindWorkspaceActionSpec(item.action);
    if (action == nullptr || action->label.empty()) {
      return;
    }
    if (EndsWithEllipsis(item.label) == EndsWithEllipsis(action->label)) {
      return;
    }
    mismatches += "\n  ";
    mismatches += menu;
    mismatches += " shows \"";
    mismatches += item.label;
    mismatches += "\" but the command palette offers \"";
    mismatches += action->label;
    mismatches += "\"";
  });
  Expect(mismatches.empty(),
         std::string("a menu entry and its palette command must agree on whether the command "
                     "prompts for more input:") +
             mismatches);
}

void TestLabelsAreCleanlySpaced() {
  std::string offenders;
  const auto check = [&offenders](std::string_view where, std::string_view label) {
    if (label.empty()) {
      return;
    }
    const bool padded = label.front() == ' ' || label.back() == ' ';
    const bool doubled = label.find("  ") != std::string_view::npos;
    if (!padded && !doubled) {
      return;
    }
    offenders += "\n  ";
    offenders += where;
    offenders += ": \"";
    offenders += label;
    offenders += "\"";
  };
  for (const ActionSpec& spec : WorkspaceCommandSpecs()) {
    check("command title", spec.label);
  }
  ForEachMenuItem([&](std::string_view menu, const MenuItemSpec& item) {
    check(menu, ResolvedLabel(item));
  });
  Expect(offenders.empty(),
         std::string("labels must carry no padding or doubled spaces:") + offenders);
}

void TestEveryMenuItemResolvesToALabel() {
  std::string blanks;
  ForEachMenuItem([&](std::string_view menu, const MenuItemSpec& item) {
    if (item.submenu != MenuId::None || !ResolvedLabel(item).empty()) {
      return;
    }
    if (item.action == ActionId::DebugBreakpointToggleEnabled ||
        item.action == ActionId::GitStageToggleEntry) {
      return;
    }
    blanks += "\n  ";
    blanks += menu;
    blanks += " has an item with no label and no registry title";
  });
  Expect(blanks.empty(), std::string("every menu row must resolve to a label:") + blanks);
}

void TestUsageRequiresArgumentOnlyOutsideBrackets() {
  using microide::workspace::CommandUsageRequiresArgument;
  Expect(CommandUsageRequiresArgument("git-create-branch <name> [start-point]"),
         "<name> is required");
  Expect(!CommandUsageRequiresArgument("git-switch-branch [branch]"), "[branch] is optional");
  Expect(!CommandUsageRequiresArgument("indent-width [n]"), "[n] is optional");
  Expect(!CommandUsageRequiresArgument("tool [--type <adapter>]"),
         "a <...> inside brackets is optional");
  Expect(CommandUsageRequiresArgument("debug-run [--type <adapter>] <program> [args...]"),
         "<program> after a bracket group is required");
  Expect(!CommandUsageRequiresArgument("goto [line[:col]]"), "nested brackets stay optional");
  for (const std::string_view bare : {"goto", "open", "files", "search"}) {
    const auto* spec = microide::workspace::FindWorkspaceActionByCommand(bare);
    Expect(spec != nullptr && !CommandUsageRequiresArgument(spec->command_usage),
           std::string(bare) + " runs bare, so its usage should not require an argument");
  }
}

}

void RegisterCommandLabelConsistencyTests(std::vector<TestCase>& tests) {
  AddTest(tests, "CommandLabels/NoAsciiEllipsis", TestNoUserFacingLabelUsesAsciiEllipsis);
  AddTest(tests, "CommandLabels/MenuAndPaletteAgreeOnEllipsis",
          TestMenuAndPaletteAgreeOnTheEllipsisPromise);
  AddTest(tests, "CommandLabels/CleanlySpaced", TestLabelsAreCleanlySpaced);
  AddTest(tests, "CommandLabels/UsageRequiresArgumentOnlyOutsideBrackets",
          TestUsageRequiresArgumentOnlyOutsideBrackets);
  AddTest(tests, "CommandLabels/EveryMenuItemResolvesToALabel",
          TestEveryMenuItemResolvesToALabel);
}

}
