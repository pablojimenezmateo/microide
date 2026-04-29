#pragma once

#include <cstddef>

#include "plugin/PluginHost.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::lua_interop {

#if MICROIDE_HAS_LUA_PLUGINS
void PushPosition(lua_State* state, std::size_t line, std::size_t column);
void PushRange(lua_State* state,
               std::size_t start_line,
               std::size_t start_column,
               std::size_t end_line,
               std::size_t end_column);
void PushHoverPosition(lua_State* state, std::size_t line, std::size_t column);
PluginHost::SidebarItem ReadSidebarItem(lua_State* state, int table_index);
#endif

}  // namespace microide::plugin::lua_interop
