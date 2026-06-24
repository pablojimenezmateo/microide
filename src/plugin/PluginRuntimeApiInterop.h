#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"

namespace microide::plugin::runtime_api_interop {

#if MICROIDE_HAS_LUA_PLUGINS
int LuaDiagnosticsPublish(lua_State* state,
                          const runtime_types::PluginInstance* plugin,
                          const std::filesystem::path& current_project_root,
                          const PluginHost::Callbacks& callbacks);

int LuaDiagnosticsClear(
    lua_State* state,
    const runtime_types::PluginInstance* plugin,
    const std::optional<std::filesystem::path>& path,
    const PluginHost::Callbacks& callbacks);

int LuaDecorationsSet(lua_State* state,
                      const runtime_types::PluginInstance* plugin,
                      const std::filesystem::path& current_project_root,
                      const PluginHost::Callbacks& callbacks);

int LuaDecorationsClear(
    lua_State* state,
    const runtime_types::PluginInstance* plugin,
    const std::optional<std::filesystem::path>& path,
    const PluginHost::Callbacks& callbacks);

int LuaSidebarShow(lua_State* state, const PluginHost::Callbacks& callbacks);
#endif

}  // namespace microide::plugin::runtime_api_interop
