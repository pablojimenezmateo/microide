#include "workspace/WorkspaceSidebarRegistry.h"

#include <algorithm>
#include <array>

#include "workspace/WorkspaceCommandParsing.h"

namespace microide::workspace {

namespace {

MenuItemSpec SidebarModeMenuItem(const SidebarToolSpec& tool) {
  MenuItemSpec item{};
  item.action = ActionId::SidebarShow;
  item.label = tool.label;
  item.args = std::array<std::string_view, 2>{tool.command_name, {}};
  item.arg_count = 1;
  item.checkable = true;
  return item;
}

}  // namespace

std::span<const SidebarToolSpec> BuiltinSidebarToolSpecs() {
  static const auto kSpecs = std::to_array<SidebarToolSpec>({
      SidebarToolSpec{"tree", "Project", WorkspaceShell::SidebarMode::Tree},
      SidebarToolSpec{"search", "Search", WorkspaceShell::SidebarMode::Search},
      SidebarToolSpec{"problems", "Problems", WorkspaceShell::SidebarMode::Problems},
      SidebarToolSpec{"git", "Source Control", WorkspaceShell::SidebarMode::Git},
  });
  return kSpecs;
}

const SidebarToolSpec* FindBuiltinSidebarTool(std::string_view command_name) {
  const auto specs = BuiltinSidebarToolSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(),
                               [command_name](const SidebarToolSpec& spec) {
                                 return spec.command_name == command_name;
                               });
  return it == specs.end() ? nullptr : &(*it);
}

const SidebarToolSpec* FindBuiltinSidebarTool(WorkspaceShell::SidebarMode mode) {
  const auto specs = BuiltinSidebarToolSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(),
                               [mode](const SidebarToolSpec& spec) {
                                 return spec.mode == mode;
                               });
  return it == specs.end() ? nullptr : &(*it);
}

const std::vector<std::string>& BuiltinSidebarToolNames() {
  static const std::vector<std::string> kNames = {
      "git",
      "problems",
      "search",
      "tree",
  };
  return kNames;
}

SidebarToolRequest ParseBuiltinSidebarToolRequest(const std::vector<std::string>& args) {
  SidebarToolRequest request;
  if (args.empty()) {
    return request;
  }

  request.tool = FindBuiltinSidebarTool(args.front());
  if (request.tool == nullptr) {
    return request;
  }

  switch (request.tool->mode) {
    case WorkspaceShell::SidebarMode::Tree:
      request.root =
          args.size() > 1 ? std::filesystem::path(args[1]) : std::filesystem::path{};
      break;
    case WorkspaceShell::SidebarMode::Search:
      request.query = JoinCommandArguments(args, 1);
      break;
    case WorkspaceShell::SidebarMode::Problems:
    case WorkspaceShell::SidebarMode::Git:
    case WorkspaceShell::SidebarMode::None:
    case WorkspaceShell::SidebarMode::Plugin:
      break;
  }

  return request;
}

std::span<const MenuItemSpec> BuiltinSidebarModeMenuItems() {
  static const auto kItems = [] {
    const SidebarToolSpec* tree = FindBuiltinSidebarTool(WorkspaceShell::SidebarMode::Tree);
    const SidebarToolSpec* search = FindBuiltinSidebarTool(WorkspaceShell::SidebarMode::Search);
    const SidebarToolSpec* problems =
        FindBuiltinSidebarTool(WorkspaceShell::SidebarMode::Problems);
    const SidebarToolSpec* git = FindBuiltinSidebarTool(WorkspaceShell::SidebarMode::Git);
    return std::to_array<MenuItemSpec>({
        SidebarModeMenuItem(*tree),
        SidebarModeMenuItem(*search),
        SidebarModeMenuItem(*problems),
        SidebarModeMenuItem(*git),
    });
  }();
  return kItems;
}

}  // namespace microide::workspace
