#include "plugin/PluginStatusInterop.h"

namespace microide::plugin::status_interop {

bool UpdateStatusItem(
    std::string_view id,
    std::string text,
    std::string tooltip,
    std::unordered_map<std::string, PluginHost::ContributedStatusItem>* status_items,
    std::vector<PluginHost::ContributedStatusItem>* status_item_order,
    const std::function<void()>& request_status_redraw) {
  if (status_items == nullptr || status_item_order == nullptr) {
    return false;
  }

  auto it = status_items->find(std::string(id));
  if (it == status_items->end()) {
    return false;
  }
  it->second.text = std::move(text);
  if (!tooltip.empty()) {
    it->second.tooltip = std::move(tooltip);
  }
  for (auto& order_item : *status_item_order) {
    if (order_item.id == it->first) {
      order_item.text = it->second.text;
      order_item.tooltip = it->second.tooltip;
      break;
    }
  }
  if (request_status_redraw) {
    request_status_redraw();
  }
  return true;
}

}  // namespace microide::plugin::status_interop
