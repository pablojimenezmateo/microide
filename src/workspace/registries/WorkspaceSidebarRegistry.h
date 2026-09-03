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

// The mode a view id resolves to, or None for an empty/unknown id. One
// definition — the shell and the sidebar coordinator each carried an identical
// private spelling of this lookup.
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

// Per-view user policy: visibility and display order.
struct SidebarViewPolicy {
  std::string view_id;
  bool hidden = false;
  int order = 0;  // lower = earlier in the list
};

// Inline capacity covers the built-ins plus a handful of plugin views, which is
// every real case; past that it spills like any vector. `SidebarModeRow` calls
// the function below on the frame path, and it was two heap vectors per painted
// frame for a list of string_views (TD-2026-08-14-221).
using OrderedSidebarViewList = util::SmallVector<SidebarViewInfo, 16>;

// Returns all views merged, ordered by policy, with hidden views removed.
// Views with no explicit policy entry keep their default order after policy-ordered views.
OrderedSidebarViewList OrderedSidebarViews(const plugin::PluginHost& plugin_host,
                                           const std::vector<SidebarViewPolicy>& policies);

// Returns the effective policy for a given view id (or a default policy with hidden=false).
SidebarViewPolicy EffectiveSidebarViewPolicy(
    std::string_view view_id,
    const std::vector<SidebarViewPolicy>& policies);

}  // namespace microide::workspace
