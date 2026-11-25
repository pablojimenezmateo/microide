#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "util/SmallVector.h"
#include "workspace/registries/WorkspaceMenuRegistry.h"
#include "workspace/state/WorkspaceSidebarState.h"

namespace microide::workspace {

struct SidebarViewSpec {
  std::string_view id;
  std::string_view label;
  SidebarMode mode = SidebarMode::None;
  bool contextual = false;
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

inline SidebarMode SidebarModeForViewId(std::string_view view_id,
                                        const plugin::PluginHost& plugin_host) {
  if (view_id.empty()) {
    return SidebarMode::None;
  }
  const auto view = FindSidebarView(view_id, plugin_host);
  return view.has_value() ? view->mode : SidebarMode::None;
}
std::vector<std::string> SidebarViewIds(const plugin::PluginHost& plugin_host);
SidebarViewRequest ParseSidebarViewRequest(const std::vector<std::string>& args,
                                          const plugin::PluginHost& plugin_host);

struct SidebarViewPolicy {
  std::string view_id;
  bool hidden = false;
  int order = 0;
};

using OrderedSidebarViewList = util::SmallVector<SidebarViewInfo, 16>;

OrderedSidebarViewList OrderedSidebarViews(const plugin::PluginHost& plugin_host,
                                           const std::vector<SidebarViewPolicy>& policies);

SidebarViewPolicy EffectiveSidebarViewPolicy(
    std::string_view view_id,
    const std::vector<SidebarViewPolicy>& policies);

}
