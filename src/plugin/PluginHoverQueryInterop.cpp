#include "plugin/PluginHoverQueryInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <unordered_map>

namespace microide::plugin::hover_query_interop {

bool QueryHover(
    const std::vector<std::string>& hover_provider_order,
    const std::unordered_map<std::string, runtime_types::HoverProvider>& hovers,
    const std::filesystem::path& resolved_path,
    std::size_t line,
    std::size_t column,
    const std::function<bool(const runtime_types::HoverProvider&,
                             const std::filesystem::path&,
                             std::size_t,
                             std::size_t,
                             PluginHost::HoverResult*,
                             std::string*)>& query_hover_provider,
    PluginHost::HoverResult* result,
    std::string* error_message) {
  for (const std::string& provider_id : hover_provider_order) {
    const auto it = hovers.find(provider_id);
    if (it == hovers.end()) {
      continue;
    }
    if (!query_hover_provider(it->second, resolved_path, line, column, result, error_message)) {
      return false;
    }
    if (!result->title.empty() || !result->content.empty()) {
      if (error_message != nullptr) {
        error_message->clear();
      }
      return true;
    }
  }

  if (error_message != nullptr) {
    error_message->clear();
  }
  return false;
}

}  // namespace microide::plugin::hover_query_interop

#endif
