#include "workspace/WorkspaceStatusRegistry.h"

#include <algorithm>
#include <string_view>

#include "plugin/PluginHost.h"

namespace microide::workspace {

namespace {

StatusItemTone ToneFromString(std::string_view tone) {
  if (tone == "error") {
    return StatusItemTone::Error;
  }
  if (tone == "warning") {
    return StatusItemTone::Warning;
  }
  if (tone == "info") {
    return StatusItemTone::Info;
  }
  return StatusItemTone::Default;
}

}  // namespace

std::vector<StatusItemView> ResolveStatusItems(const plugin::PluginHost& plugin_host) {
  return ResolveStatusItems(plugin_host.ContributedStatusItems());
}

std::vector<StatusItemView> ResolveStatusItems(
    const std::vector<plugin::PluginHost::ContributedStatusItem>& contributions) {
  std::vector<StatusItemView> items;
  if (contributions.empty()) {
    return items;  // No plugin contributes status items: zero allocation, no sort.
  }
  items.reserve(contributions.size());
  for (const auto& contrib : contributions) {
    items.push_back(StatusItemView{
        .id = contrib.id,
        .text = contrib.text,
        .tooltip = contrib.tooltip,
        .alignment =
            (contrib.alignment == "left") ? StatusAlignment::Left : StatusAlignment::Right,
        .priority = contrib.priority,
        .icon = contrib.icon,
        .tone = ToneFromString(contrib.tone),
        .command = contrib.command,
        .progress = contrib.progress,
        .plugin_id = contrib.plugin_id,
    });
  }
  // stable_sort so items with equal alignment+priority keep their registration
  // (contribution) order instead of reordering nondeterministically between
  // revisions/platforms — plain std::sort has no tie-break and would jitter.
  std::stable_sort(items.begin(), items.end(),
                   [](const StatusItemView& a, const StatusItemView& b) {
                     if (a.alignment != b.alignment) {
                       return a.alignment < b.alignment;
                     }
                     return a.priority > b.priority;
                   });
  return items;
}

const std::vector<StatusItemView>& ResolveStatusItems(const plugin::PluginHost& plugin_host,
                                                      StatusItemCache& cache) {
  const std::uint64_t revision = plugin_host.StatusItemsRevision();
  if (cache.revision != revision) {
    cache.items = ResolveStatusItems(plugin_host);
    cache.revision = revision;
  }
  return cache.items;
}

}  // namespace microide::workspace
