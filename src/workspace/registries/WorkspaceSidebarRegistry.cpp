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
      break;
  }

  return request;
}

OrderedSidebarViewList OrderedSidebarViews(const plugin::PluginHost& plugin_host,
                                           const std::vector<SidebarViewPolicy>& policies) {
  // First entry wins, matching EffectiveSidebarViewPolicy. This used to pre-index
  // `policies` into an `unordered_map` to avoid re-scanning it inside every
  // filter/sort comparison — but the sort no longer resolves anything (the order
  // is cached per view below), so the scan is once per view, and the map was one
  // heap node per policy plus a bucket array **per call**. `SidebarModeRow` runs
  // this on the frame path, so that was ~10 allocations a frame on every scroll
  // scenario (TD-2026-08-06-159). Both sides are bounded by the sidebar view set
  // — a handful of built-ins plus the capped plugin contributions — so a linear
  // scan is a few dozen `string_view` compares against no allocation at all.
  //
  // Returns only the two fields the caller reads. Returning a `SidebarViewPolicy`
  // by value meant a `std::string` copy per view, and the not-found branch built
  // one from the id purely to fill a field nobody looked at.
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

  // Resolve each surviving view's effective order once, then stable-sort on the
  // cached key (views with no explicit policy entry sort at order = INT_MAX).
  //
  // Built inline from the two sources rather than from `SidebarViews(...)`: that
  // helper materialises a whole third vector this function would immediately
  // filter and drop, and `SidebarViewInfo` is two string_views and an enum, so
  // there is nothing to reuse from it.
  struct Ordered {
    SidebarViewInfo info;
    int order;
  };
  const auto builtins = BuiltinSidebarViewSpecs();
  const auto& plugin_providers = plugin_host.SidebarProviders();
  // Both lists are the same bounded shape and both used to be heap vectors, on a
  // path that runs once per painted frame (TD-2026-08-14-221).
  util::SmallVector<Ordered, 16> ordered;
  ordered.reserve(builtins.size() + plugin_providers.size());
  const auto consider = [&](SidebarViewInfo info) {
    const ResolvedPolicy policy = resolve(info.id);
    if (policy.hidden) {
      return;
    }
    ordered.push_back(Ordered{info, policy.order});
  };
  for (const SidebarViewSpec& spec : builtins) {
    consider(SidebarViewInfo{.id = spec.id, .label = spec.label, .mode = spec.mode});
  }
  for (const auto& provider : plugin_providers) {
    consider(SidebarViewInfo{
        .id = provider.id, .label = provider.label, .mode = SidebarMode::Plugin});
  }
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const Ordered& a, const Ordered& b) { return a.order < b.order; });

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

}  // namespace microide::workspace
