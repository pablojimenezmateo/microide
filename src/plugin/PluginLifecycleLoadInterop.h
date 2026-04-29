#pragma once

#include <string>

#include "plugin/PluginHostRuntimeTypes.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::lifecycle_load_interop {

#if MICROIDE_HAS_LUA_PLUGINS
bool InitializeState(runtime_types::PluginInstance* plugin,
                     lua_CFunction open_microide,
                     std::string* error_message);

bool LoadPluginDescriptor(runtime_types::PluginInstance* plugin, std::string* error_message);
#endif

}  // namespace microide::plugin::lifecycle_load_interop
