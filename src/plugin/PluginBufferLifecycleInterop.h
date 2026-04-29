#pragma once

#include <filesystem>
#include <functional>
#include <vector>

#include "plugin/PluginHostRuntimeTypes.h"

namespace microide::plugin::buffer_lifecycle_interop {

#if MICROIDE_HAS_LUA_PLUGINS
void DispatchBufferCallbacks(
    std::vector<runtime_types::PluginInstance>* plugins,
    int runtime_types::PluginInstance::* callback_ref,
    const char* callback_name,
    const std::filesystem::path& path,
    const std::function<void(runtime_types::PluginInstance*,
                             int,
                             const char*,
                             const std::filesystem::path&)>& call_buffer_callback);
#endif

}  // namespace microide::plugin::buffer_lifecycle_interop
