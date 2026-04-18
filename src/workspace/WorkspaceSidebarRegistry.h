#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceMenuRegistry.h"
#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

struct SidebarToolSpec {
  std::string_view command_name;
  std::string_view label;
  WorkspaceShell::SidebarMode mode = WorkspaceShell::SidebarMode::None;
};

struct SidebarToolRequest {
  const SidebarToolSpec* tool = nullptr;
  std::filesystem::path root;
  std::string query;
};

std::span<const SidebarToolSpec> BuiltinSidebarToolSpecs();
const SidebarToolSpec* FindBuiltinSidebarTool(std::string_view command_name);
const SidebarToolSpec* FindBuiltinSidebarTool(WorkspaceShell::SidebarMode mode);
const std::vector<std::string>& BuiltinSidebarToolNames();
SidebarToolRequest ParseBuiltinSidebarToolRequest(const std::vector<std::string>& args);
std::span<const MenuItemSpec> BuiltinSidebarModeMenuItems();

}  // namespace microide::workspace
