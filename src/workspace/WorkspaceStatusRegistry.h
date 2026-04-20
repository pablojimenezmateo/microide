#pragma once

#include <string>
#include <vector>

namespace microide::plugin {
class PluginHost;
}  // namespace microide::plugin

namespace microide::workspace {

enum class StatusAlignment { Left, Right };

struct StatusItemView {
  std::string id;
  std::string text;
  std::string tooltip;
  StatusAlignment alignment = StatusAlignment::Right;
  int priority = 0;  // higher = rendered closer to the edge
  std::string plugin_id;
};

// Returns plugin-contributed status items, sorted by alignment then priority descending.
std::vector<StatusItemView> ResolveStatusItems(const plugin::PluginHost& plugin_host);

}  // namespace microide::workspace
