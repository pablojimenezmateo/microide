#pragma once

#include <filesystem>
#include <memory>

#include "plugin/PluginHostRuntimeTypes.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::process_interop {

#if MICROIDE_HAS_LUA_PLUGINS
int LuaProcessRun(lua_State* state, const std::filesystem::path& current_project_root);

int LuaProcessRunAsync(lua_State* state,
                       const std::filesystem::path& current_project_root,
                       std::shared_ptr<runtime_types::AsyncProcessState> async_process_state);
#endif

}  // namespace microide::plugin::process_interop
