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
// Both entry points enforce the calling plugin's process capability (exec flag, argv[0]
// allowlist, and cwd containment) before spawning. On refusal they push a static error
// message and return lua_error_util::kPendingError for the wrapper to raise.
int LuaProcessRun(lua_State* state, const PluginFsContext& fs);

int LuaProcessRunAsync(lua_State* state,
                       const PluginFsContext& fs,
                       std::shared_ptr<runtime_types::AsyncProcessState> async_process_state);
#endif

}  // namespace microide::plugin::process_interop
