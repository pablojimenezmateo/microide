#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "workspace/StatusTone.h"
#include "plugin/PluginHost.h"

namespace microide::workspace {

enum class StatusAlignment { Left, Right };


struct StatusItemView {
  std::string id;
  std::string text;
  std::string tooltip;
  StatusAlignment alignment = StatusAlignment::Right;
  int priority = 0;
  std::string icon;
  StatusTone tone = StatusTone::Default;
  std::string command;
  float progress = -1.0f;
  std::string plugin_id;
};

std::vector<StatusItemView> ResolveStatusItems(const plugin::PluginHost& plugin_host);

std::vector<StatusItemView> ResolveStatusItems(
    const std::vector<plugin::PluginHost::ContributedStatusItem>& contributions);

struct StatusItemCache {
  std::vector<StatusItemView> items;
  std::uint64_t revision = std::numeric_limits<std::uint64_t>::max();
};

const std::vector<StatusItemView>& ResolveStatusItems(const plugin::PluginHost& plugin_host,
                                                      StatusItemCache& cache);

}
