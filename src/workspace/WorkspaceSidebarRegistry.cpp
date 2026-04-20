#include "workspace/WorkspaceSidebarRegistry.h"

#include <algorithm>
#include <array>
#include <limits>

#include "workspace/WorkspaceCommandParsing.h"

namespace microide::workspace {

std::span<const SidebarViewSpec> BuiltinSidebarViewSpecs() {
  static const auto kSpecs = std::to_array<SidebarViewSpec>({
      SidebarViewSpec{"tree", "Project", SidebarMode::Tree},
      SidebarViewSpec{"search", "Search", SidebarMode::Search},
      SidebarViewSpec{"problems", "Problems", SidebarMode::Problems},
      SidebarViewSpec{"git", "Source Control", SidebarMode::Git},
  });
  return kSpecs;
}

const SidebarViewSpec* FindBuiltinSidebarView(std::string_view id) {
  const auto specs = BuiltinSidebarViewSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(),
                               [id](const SidebarViewSpec& spec) {
                                 return spec.id == id;
                               });
  return it == specs.end() ? nullptr : &(*it);
}

const SidebarViewSpec* FindBuiltinSidebarView(SidebarMode mode) {
  const auto specs = BuiltinSidebarViewSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(),
                               [mode](const SidebarViewSpec& spec) {
                                 return spec.mode == mode;
                               });
  return it == specs.end() ? nullptr : &(*it);
}

std::vector<SidebarViewInfo> SidebarViews(const plugin::PluginHost& plugin_host) {
  std::vector<SidebarViewInfo> views;
  const auto builtins = BuiltinSidebarViewSpecs();
  const auto& plugin_providers = plugin_host.SidebarProviders();
  views.reserve(builtins.size() + plugin_providers.size());
  for (const SidebarViewSpec& spec : builtins) {
    views.push_back(SidebarViewInfo{
        .id = spec.id,
        .label = spec.label,
        .mode = spec.mode,
    });
  }
  for (const auto& provider : plugin_providers) {
    views.push_back(SidebarViewInfo{
        .id = provider.id,
        .label = provider.label,
        .mode = SidebarMode::Plugin,
    });
  }
  return views;
}

std::optional<SidebarViewInfo> FindSidebarView(std::string_view id,
                                               const plugin::PluginHost& plugin_host) {
  if (const SidebarViewSpec* builtin = FindBuiltinSidebarView(id); builtin != nullptr) {
    return SidebarViewInfo{
        .id = builtin->id,
        .label = builtin->label,
        .mode = builtin->mode,
    };
  }

  if (const auto* plugin_view = plugin_host.FindSidebarProvider(id); plugin_view != nullptr) {
    return SidebarViewInfo{
        .id = plugin_view->id,
        .label = plugin_view->label,
        .mode = SidebarMode::Plugin,
    };
  }

  return std::nullopt;
}

std::vector<std::string> SidebarViewIds(const plugin::PluginHost& plugin_host) {
  std::vector<std::string> ids;
  const auto views = SidebarViews(plugin_host);
  ids.reserve(views.size());
  for (const SidebarViewInfo& view : views) {
    ids.emplace_back(view.id);
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

SidebarViewRequest ParseSidebarViewRequest(const std::vector<std::string>& args,
                                          const plugin::PluginHost& plugin_host) {
  SidebarViewRequest request;
  if (args.empty()) {
    return request;
  }

  request.view = FindSidebarView(args.front(), plugin_host);
  if (!request.view.has_value()) {
    return request;
  }

  switch (request.view->mode) {
    case SidebarMode::Tree:
      request.root =
          args.size() > 1 ? std::filesystem::path(args[1]) : std::filesystem::path{};
      break;
    case SidebarMode::Search:
      request.query = JoinCommandArguments(args, 1);
      break;
    case SidebarMode::Problems:
    case SidebarMode::Git:
    case SidebarMode::None:
    case SidebarMode::Plugin:
      break;
  }

  return request;
}

std::vector<SidebarViewInfo> OrderedSidebarViews(
    const plugin::PluginHost& plugin_host,
    const std::vector<SidebarViewPolicy>& policies) {
  std::vector<SidebarViewInfo> all = SidebarViews(plugin_host);

  // Remove hidden views.
  all.erase(std::remove_if(all.begin(), all.end(),
                            [&](const SidebarViewInfo& info) {
                              return EffectiveSidebarViewPolicy(info.id, policies).hidden;
                            }),
            all.end());

  // Stable-sort by policy order (views with no explicit entry get order = INT_MAX).
  std::stable_sort(all.begin(), all.end(),
                   [&](const SidebarViewInfo& a, const SidebarViewInfo& b) {
                     return EffectiveSidebarViewPolicy(a.id, policies).order <
                            EffectiveSidebarViewPolicy(b.id, policies).order;
                   });
  return all;
}

SidebarViewPolicy EffectiveSidebarViewPolicy(std::string_view view_id,
                                              const std::vector<SidebarViewPolicy>& policies) {
  for (const SidebarViewPolicy& p : policies) {
    if (p.view_id == view_id) {
      return p;
    }
  }
  return SidebarViewPolicy{std::string(view_id), false, std::numeric_limits<int>::max()};
}

}  // namespace microide::workspace
