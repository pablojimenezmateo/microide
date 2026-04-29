#pragma once

#include <filesystem>
#include <functional>
#include <optional>

#include "plugin/PluginHost.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::lua_buffer_project_interop {

#if MICROIDE_HAS_LUA_PLUGINS
void PushProjectTable(lua_State* state, const std::filesystem::path& project_root);

void PushBufferTable(lua_State* state,
                     const std::filesystem::path& path,
                     const std::function<std::optional<std::string>(
                         const std::filesystem::path&)>& relative_path_string);
#endif

}  // namespace microide::plugin::lua_buffer_project_interop
