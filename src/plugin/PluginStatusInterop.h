#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "plugin/PluginHost.h"

namespace microide::plugin::status_interop {

bool UpdateStatusItem(
    std::string_view id,
    std::string text,
    std::string tooltip,
    std::unordered_map<std::string, PluginHost::ContributedStatusItem>* status_items,
    std::vector<PluginHost::ContributedStatusItem>* status_item_order,
    const std::function<void()>& request_status_redraw);

}  // namespace microide::plugin::status_interop
