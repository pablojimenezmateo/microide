#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace microide::plugin {
class PluginHost;
}  // namespace microide::plugin

namespace microide::workspace {

enum class StatusAlignment { Left, Right };

// Tone of a plugin status item, derived from the contributed "tone" string at
// resolve time so the render path never re-parses strings.
enum class StatusItemTone { Default, Info, Warning, Error };

struct StatusItemView {
  std::string id;
  std::string text;
  std::string tooltip;
  StatusAlignment alignment = StatusAlignment::Right;
  int priority = 0;  // higher = rendered closer to the edge
  // Phase D enrichments. `icon` is a gutter-icon name (resolved to a shape by the
  // render path); empty = none. `tone` tints the item background. `command` runs
  // on click when non-empty. `progress` < 0 means no bar, else clamped [0, 1].
  std::string icon;
  StatusItemTone tone = StatusItemTone::Default;
  std::string command;
  float progress = -1.0f;
  std::string plugin_id;
};

// Returns plugin-contributed status items, sorted by alignment then priority descending.
std::vector<StatusItemView> ResolveStatusItems(const plugin::PluginHost& plugin_host);

// Caches the resolved/sorted view so the per-frame render, hit-test, and hover
// paths reuse a single build until the host's contributions change. `revision`
// starts at an impossible value so the first resolve always rebuilds.
struct StatusItemCache {
  std::vector<StatusItemView> items;
  std::uint64_t revision = std::numeric_limits<std::uint64_t>::max();
};

// Cache-aware resolve: rebuilds (copy + parse + sort) only when the host's
// StatusItemsRevision() differs from the cached stamp; otherwise returns the
// previously built view with no allocation or sort.
const std::vector<StatusItemView>& ResolveStatusItems(const plugin::PluginHost& plugin_host,
                                                      StatusItemCache& cache);

}  // namespace microide::workspace
