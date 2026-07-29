#include "TestSupport.h"

#include "workspace/WorkspaceCommandRegistry.h"
#include "workspace/WorkspaceMenuRegistry.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

// A command has one name. It is reachable from the menu bar, from a context menu, from
// the command palette and from the Help reference, and every one of those reads the same
// registry, so nothing should be able to make them disagree — but the menu registry can
// override a label per item, and for a while several did: the palette offered "Compare
// Against HEAD" for what the Git menu called "Compare with HEAD", "New Tab" for what the
// File menu called "New File", and "About microide" for what the Help menu called "Help".
// A user who learned a command's name from a menu could not find it by that name.
//
// These tests are the guard. Most menu items now carry no label at all so they inherit
// the registry title and cannot drift; the checks below cover the ones that must keep an
// override (a category-shortened Git/Debug entry, an args-differentiated Zoom entry) and
// the punctuation conventions that span both registries.

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

constexpr std::string_view kEllipsis = "\xE2\x80\xA6";  // U+2026

const std::array<TreeContextTargetKind, 8> kTreeTargets = {
    TreeContextTargetKind::File,          TreeContextTargetKind::Directory,
    TreeContextTargetKind::Root,          TreeContextTargetKind::Background,
    TreeContextTargetKind::BreakpointLine, TreeContextTargetKind::GitEntry,
    TreeContextTargetKind::ResultRow,     TreeContextTargetKind::DebugValueRow,
};

// The label the shell paints for a menu item, resolving through the registry exactly as
// WorkspaceShell::MenuItemLabel does for the static cases.
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

// Three periods where the rest of the shell writes "…". Both spellings were in use: the
// file-tree context menu said "Rename..." / "Delete..." directly beside a git menu that
// said "Discard…", and the status bar said "LSP: Starting..." beside a Go menu that said
// "(LSP starting...)". One ellipsis, everywhere.
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

// The trailing "…" is a promise: picking this asks for more input before it acts. When
// the menu makes that promise and the palette does not (File > "Open File…" vs the
// palette's "Open File"), one of the two is lying about what the command does.
void TestMenuAndPaletteAgreeOnTheEllipsisPromise() {
  std::string mismatches;
  ForEachMenuItem([&](std::string_view menu, const MenuItemSpec& item) {
    // An item with arguments is a narrower command than its action (UiScale "Zoom In"
    // vs the palette's "UI Scale"), so its label is legitimately its own.
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

// A label is painted straight into a row, so leading/trailing space and doubled spaces
// show up as visible misalignment against its neighbours.
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

// Every actionable menu row must resolve to *some* label. An item that inherits from the
// registry paints nothing at all if that action has no title, which is a blank row rather
// than a visible error.
void TestEveryMenuItemResolvesToALabel() {
  std::string blanks;
  ForEachMenuItem([&](std::string_view menu, const MenuItemSpec& item) {
    if (item.submenu != MenuId::None || !ResolvedLabel(item).empty()) {
      return;
    }
    // The two state-flipping items resolve their label in the shell, not the registry.
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

}  // namespace

void RegisterCommandLabelConsistencyTests(std::vector<TestCase>& tests) {
  AddTest(tests, "CommandLabels/NoAsciiEllipsis", TestNoUserFacingLabelUsesAsciiEllipsis);
  AddTest(tests, "CommandLabels/MenuAndPaletteAgreeOnEllipsis",
          TestMenuAndPaletteAgreeOnTheEllipsisPromise);
  AddTest(tests, "CommandLabels/CleanlySpaced", TestLabelsAreCleanlySpaced);
  AddTest(tests, "CommandLabels/EveryMenuItemResolvesToALabel",
          TestEveryMenuItemResolvesToALabel);
}

}  // namespace microide::tests
