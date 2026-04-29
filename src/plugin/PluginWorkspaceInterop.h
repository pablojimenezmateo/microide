#pragma once

#include <filesystem>

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

int LuaFilesReadText(lua_State* state, const std::filesystem::path& current_project_root);
int LuaFilesWriteText(lua_State* state, const std::filesystem::path& current_project_root);
int LuaFilesExists(lua_State* state, const std::filesystem::path& current_project_root);
#endif

}  // namespace microide::plugin::workspace_interop
