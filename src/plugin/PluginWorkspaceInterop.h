#pragma once

#include <filesystem>

#include "plugin/PluginCapabilities.h"
#include "plugin/PluginHost.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::workspace_interop {

#if MICROIDE_HAS_LUA_PLUGINS
int LuaWorkspaceProjectRoot(lua_State* state, const std::filesystem::path& current_project_root);

int LuaWorkspaceOpenFile(lua_State* state,
                         const std::filesystem::path& current_project_root,
                         const PluginHost::Callbacks& callbacks);

int LuaWorkspaceActiveBuffer(lua_State* state,
                             const std::filesystem::path& current_project_root,
                             const PluginHost::Callbacks& callbacks);

// Sandboxed file helpers. Each enforces the calling plugin's filesystem capability and path
// containment; when an access is refused they push the falsy result (nil/false) and set
// `*denied` so the wrapper layer can surface a diagnostic. `denied` may be null.
int LuaFilesReadText(lua_State* state, const PluginFsContext& fs, bool* denied);
int LuaFilesWriteText(lua_State* state, const PluginFsContext& fs, bool* denied);
int LuaFilesExists(lua_State* state, const PluginFsContext& fs, bool* denied);
#endif

}  // namespace microide::plugin::workspace_interop
