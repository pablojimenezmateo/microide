#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceMenuRegistry.h"
#include "workspace/WorkspaceSidebarState.h"

namespace microide::workspace {

struct SidebarViewSpec {
  std::string_view id;
  std::string_view label;
  SidebarMode mode = SidebarMode::None;
};

struct SidebarViewInfo {
  std::string_view id;
  std::string_view label;
  SidebarMode mode = SidebarMode::None;
};

struct SidebarViewRequest {
  std::optional<SidebarViewInfo> view;
  std::filesystem::path root;
  std::string query;
};

std::span<const SidebarViewSpec> BuiltinSidebarViewSpecs();
const SidebarViewSpec* FindBuiltinSidebarView(std::string_view id);
const SidebarViewSpec* FindBuiltinSidebarView(SidebarMode mode);
std::vector<SidebarViewInfo> SidebarViews(const plugin::PluginHost& plugin_host);
std::optional<SidebarViewInfo> FindSidebarView(std::string_view id,
                                               const plugin::PluginHost& plugin_host);
std::vector<std::string> SidebarViewIds(const plugin::PluginHost& plugin_host);
SidebarViewRequest ParseSidebarViewRequest(const std::vector<std::string>& args,
                                          const plugin::PluginHost& plugin_host);

}  // namespace microide::workspace
