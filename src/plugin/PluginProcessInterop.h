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

// process.run_async runs on the plugin worker thread, where blocking on the
// subprocess never stalls the UI. It executes the command synchronously and then
// invokes the supplied callback on the worker (where the lua_State lives) via a
// protected call. The async detached-thread + UI-thread-callback machinery it
// replaced is gone, so a plugin callback can never touch a lua_State off-thread.
int LuaProcessRunAsync(lua_State* state, const PluginFsContext& fs);
#endif

}  // namespace microide::plugin::process_interop
