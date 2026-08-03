#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/registries/WorkspaceMenuRegistry.h"
#include "workspace/state/WorkspaceSidebarState.h"

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

// Per-view user policy: visibility and display order.
struct SidebarViewPolicy {
  std::string view_id;
  bool hidden = false;
  int order = 0;  // lower = earlier in the list
};

// Returns all views merged, ordered by policy, with hidden views removed.
// Views with no explicit policy entry keep their default order after policy-ordered views.
std::vector<SidebarViewInfo> OrderedSidebarViews(
    const plugin::PluginHost& plugin_host,
    const std::vector<SidebarViewPolicy>& policies);

// Returns the effective policy for a given view id (or a default policy with hidden=false).
SidebarViewPolicy EffectiveSidebarViewPolicy(
    std::string_view view_id,
    const std::vector<SidebarViewPolicy>& policies);

}  // namespace microide::workspace
