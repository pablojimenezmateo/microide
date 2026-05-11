#include "workspace/WorkspaceStatusRegistry.h"

#include <algorithm>

#include "plugin/PluginHost.h"

namespace microide::workspace {

std::vector<StatusItemView> ResolveStatusItems(const plugin::PluginHost& plugin_host) {
  std::vector<StatusItemView> items;
  for (const auto& contrib : plugin_host.ContributedStatusItems()) {
    items.push_back(StatusItemView{
        .id = contrib.id,
        .text = contrib.text,
        .tooltip = contrib.tooltip,
        .alignment =
            (contrib.alignment == "left") ? StatusAlignment::Left : StatusAlignment::Right,
        .priority = contrib.priority,
        .plugin_id = contrib.plugin_id,
    });
  }
  std::sort(items.begin(), items.end(), [](const StatusItemView& a, const StatusItemView& b) {
    if (a.alignment != b.alignment) {
      return a.alignment < b.alignment;
    }
    return a.priority > b.priority;
  });
  return items;
}

}  // namespace microide::workspace
