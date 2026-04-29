#pragma once

#include <functional>
#include <string>

#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"

namespace microide::plugin::async_callback_interop {

#if MICROIDE_HAS_LUA_PLUGINS
int ConsumeCallbacks(
    runtime_types::AsyncProcessState& async_process_state,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(std::string)>& error_sink);
#endif

}  // namespace microide::plugin::async_callback_interop
