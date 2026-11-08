// The `surface` query: everything a person can reach, enumerated from the app's own
// registries, so the QA monkey's coverage is measured against an inventory that
// cannot drift from the code (dev-docs/project/safety-plan.md W1.7).

#include <array>
#include <string>
#include <string_view>

#include "workspace/control/ControlChannelService.h"
#include "workspace/control/ControlProtocol.h"
#include "workspace/control/ControlUiQuery.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"
#include "workspace/registries/WorkspaceMenuRegistry.h"
#include "workspace/registries/WorkspaceSidebarRegistry.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

namespace {

// The kinds named by the `ui` query. Listed by hand: an enum has no iteration,
// and these change rarely; ControlUiQuery's name functions are the spelling.
constexpr std::array kOverlayModes = {OverlayMode::FileFinder,     OverlayMode::BufferSearch,
                                      OverlayMode::BufferReplace,  OverlayMode::ProjectSearch,
                                      OverlayMode::CommitPicker,   OverlayMode::QuickPick,
                                      OverlayMode::CommandPalette, OverlayMode::Completion,
                                      OverlayMode::CodeActions};
constexpr std::array kPanelContents = {PanelContentKind::Terminal, PanelContentKind::Output,
                                       PanelContentKind::PluginSurface};
constexpr std::array kContextTargets = {
    TreeContextTargetKind::File,       TreeContextTargetKind::Directory,      TreeContextTargetKind::Root,
    TreeContextTargetKind::Background, TreeContextTargetKind::BreakpointLine, TreeContextTargetKind::GitEntry,
    TreeContextTargetKind::ResultRow,  TreeContextTargetKind::DebugValueRow};
constexpr std::array<std::string_view, 8> kContextTargetNames = {
    "file", "directory", "root", "background", "breakpoint-line", "git-entry", "result-row", "debug-value-row"};

util::JsonValue MenuItems(std::span<const MenuItemSpec> items) {
  util::JsonArray out;
  for (const MenuItemSpec& item : items) {
    if (item.separator) {
      continue;
    }
    // A row with no label of its own paints its action's (the menu registry's
    // MenuItem(ActionId) form), and runs its action's command.
    const ActionSpec* spec = FindWorkspaceActionSpec(item.action);
    const std::string_view label = !item.label.empty() ? item.label : spec != nullptr ? spec->label : "";
    const std::string_view command =
        !item.command_name.empty() ? item.command_name : spec != nullptr ? spec->command_name : "";
    if (label.empty()) {
      continue;
    }
    util::JsonObject row;
    row["label"] = util::JsonValue(std::string(label));
    if (!command.empty()) {
      row["command"] = util::JsonValue(std::string(command));
    }
    if (item.submenu != MenuId::None) {
      if (const MenuSpec* sub = FindWorkspaceMenuSpec(item.submenu); sub != nullptr) {
        row["submenu"] = util::JsonValue(std::string(sub->label));
      }
    }
    out.push_back(util::JsonValue(std::move(row)));
  }
  return util::JsonValue(std::move(out));
}

util::JsonValue Strings(auto&& values) {
  util::JsonArray out;
  for (const auto& value : values) {
    out.push_back(util::JsonValue(std::string(value)));
  }
  return util::JsonValue(std::move(out));
}

}  // namespace

util::JsonValue ControlChannelService::BuildSurface() const {
  util::JsonObject object;
  // Each answer is a named local: a range-for over `BuildCommands().AsArray()`
  // iterates a temporary destroyed before the loop body runs (no C++23
  // lifetime extension under -std=c++20).
  const util::JsonValue command_rows = BuildCommands();
  util::JsonArray commands;
  for (const util::JsonValue& command : command_rows.AsArray()) {
    commands.push_back(command["command"]);
  }
  object["commands"] = util::JsonValue(std::move(commands));
  const util::JsonValue binding_rows = BuildKeybindings();
  util::JsonArray keybindings;
  for (const util::JsonValue& binding : binding_rows.AsArray()) {
    util::JsonObject row;
    row["id"] = binding["id"];
    row["chord"] = binding["chord"];
    row["context"] = binding["context"];
    keybindings.push_back(util::JsonValue(std::move(row)));
  }
  object["keybindings"] = util::JsonValue(std::move(keybindings));
  const util::JsonValue setting_rows = BuildSettings();
  util::JsonArray settings;
  for (const util::JsonValue& setting : setting_rows.AsArray()) {
    settings.push_back(setting["id"]);
  }
  object["settings"] = util::JsonValue(std::move(settings));
  util::JsonObject menus;
  for (const MenuSpec& menu : WorkspaceMenuSpecs()) {
    menus[std::string(menu.label)] = MenuItems(menu.items);
  }
  object["menus"] = util::JsonValue(std::move(menus));
  util::JsonObject context_menus;
  for (std::size_t i = 0; i < kContextTargets.size(); ++i) {
    context_menus[std::string(kContextTargetNames[i])] = MenuItems(WorkspaceTreeContextMenuItems(kContextTargets[i]));
  }
  object["contextMenus"] = util::JsonValue(std::move(context_menus));
  util::JsonArray sidebar_views;
  for (const SidebarViewSpec& view : BuiltinSidebarViewSpecs()) {
    sidebar_views.push_back(util::JsonValue(std::string(view.id)));
  }
  object["sidebarViews"] = util::JsonValue(std::move(sidebar_views));
  util::JsonArray overlay_modes;
  for (OverlayMode mode : kOverlayModes) {
    overlay_modes.push_back(util::JsonValue(std::string(ControlUiOverlayModeName(mode))));
  }
  object["overlayModes"] = util::JsonValue(std::move(overlay_modes));
  util::JsonArray panel_contents;
  for (PanelContentKind content : kPanelContents) {
    panel_contents.push_back(util::JsonValue(std::string(ControlUiPanelContentName(content))));
  }
  object["panelContents"] = util::JsonValue(std::move(panel_contents));
  object["tabKinds"] = Strings(std::array<std::string_view, 3>{"editor", "compare", "merge"});
  object["focusTargets"] = Strings(std::array<std::string_view, 6>{"editor", "sidebar", "panel", "overlay",
                                                                    "debug-pane", "terminal"});
  object["queries"] = Strings(ControlQueryVerbs());
  return util::JsonValue(std::move(object));
}

}  // namespace microide::workspace
