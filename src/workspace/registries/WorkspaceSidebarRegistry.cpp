#include "workspace/registries/WorkspaceSidebarRegistry.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

#include "workspace/WorkspaceCommandParsing.h"

namespace microide::workspace {

std::span<const SidebarViewSpec> BuiltinSidebarViewSpecs() {
  static const auto kSpecs = std::to_array<SidebarViewSpec>({
      SidebarViewSpec{"tree", "Project", SidebarMode::Tree},
      SidebarViewSpec{"search", "Search", SidebarMode::Search},
      SidebarViewSpec{"git", "Source Control", SidebarMode::Git},
      SidebarViewSpec{"problems", "Problems", SidebarMode::Problems},
      SidebarViewSpec{"tests", "Tests", SidebarMode::Tests},
      SidebarViewSpec{"outline", "Outline", SidebarMode::Outline},
      SidebarViewSpec{"host-changes", "Changed on Host", SidebarMode::HostChanges,
                      true},
      SidebarViewSpec{"host", "Host", SidebarMode::Host, true},
      SidebarViewSpec{"conflicts", "Conflicts", SidebarMode::Conflicts, true},
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
    case SidebarMode::Git:
    case SidebarMode::None:
    case SidebarMode::Plugin:
    case SidebarMode::Problems:
    case SidebarMode::Tests:
    case SidebarMode::Outline:
    case SidebarMode::HostChanges:
    case SidebarMode::Host:
    case SidebarMode::Conflicts:
      break;
  }

  return request;
}

OrderedSidebarViewList OrderedSidebarViews(const plugin::PluginHost& plugin_host,
                                           const std::vector<SidebarViewPolicy>& policies) {
  struct ResolvedPolicy {
    bool hidden;
    int order;
  };
  const auto resolve = [&](std::string_view id) -> ResolvedPolicy {
    for (const SidebarViewPolicy& p : policies) {
      if (p.view_id == id) {
        return ResolvedPolicy{p.hidden, p.order};
      }
    }
    return ResolvedPolicy{false, std::numeric_limits<int>::max()};
  };

  struct Ordered {
    SidebarViewInfo info;
    int order;
    std::size_t index;
  };
  const auto builtins = BuiltinSidebarViewSpecs();
  const auto& plugin_providers = plugin_host.SidebarProviders();
  util::SmallVector<Ordered, 16> ordered;
  ordered.reserve(builtins.size() + plugin_providers.size());
  const auto consider = [&](SidebarViewInfo info) {
    const ResolvedPolicy policy = resolve(info.id);
    if (policy.hidden) {
      return;
    }
    ordered.push_back(Ordered{info, policy.order, ordered.size()});
  };
  for (const SidebarViewSpec& spec : builtins) {
    if (!spec.contextual) {
      consider(SidebarViewInfo{.id = spec.id, .label = spec.label, .mode = spec.mode});
    }
  }
  for (const auto& provider : plugin_providers) {
    consider(SidebarViewInfo{
        .id = provider.id, .label = provider.label, .mode = SidebarMode::Plugin});
  }
  std::sort(ordered.begin(), ordered.end(), [](const Ordered& a, const Ordered& b) {
    return a.order != b.order ? a.order < b.order : a.index < b.index;
  });

  OrderedSidebarViewList result;
  result.reserve(ordered.size());
  for (const Ordered& entry : ordered) {
    result.push_back(entry.info);
  }
  return result;
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

}
