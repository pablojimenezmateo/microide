#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

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

void PushBufferTextMethods(lua_State* state, const std::shared_ptr<const std::string>& text);
#endif

}
