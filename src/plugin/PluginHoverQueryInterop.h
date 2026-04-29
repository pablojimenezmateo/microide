#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"

namespace microide::plugin::hover_query_interop {

#if MICROIDE_HAS_LUA_PLUGINS
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
    std::string* error_message);
#endif

}  // namespace microide::plugin::hover_query_interop
