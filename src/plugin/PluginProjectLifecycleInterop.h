#pragma once

#include <functional>
#include <vector>

#include "plugin/PluginHostRuntimeTypes.h"

namespace microide::plugin::project_lifecycle_interop {

#if MICROIDE_HAS_LUA_PLUGINS
void DispatchProjectOpenCallbacks(
    std::vector<runtime_types::PluginInstance>* plugins,
    const std::function<void(runtime_types::PluginInstance*, int, const char*)>&
        call_project_callback);
#endif

}  // namespace microide::plugin::project_lifecycle_interop
