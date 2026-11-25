#pragma once

#include <filesystem>
#include <memory>

#include "plugin/PluginCapabilities.h"
#include "plugin/PluginHostRuntimeTypes.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::process_interop {

#if MICROIDE_HAS_LUA_PLUGINS
int LuaProcessRun(lua_State* state, const PluginFsContext& fs);

int LuaProcessRunAsync(lua_State* state, const PluginFsContext& fs);
#endif

}
