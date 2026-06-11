#pragma once

#include <cstddef>

#include "plugin/PluginHost.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::lua_interop {

#if MICROIDE_HAS_LUA_PLUGINS
// Restores the Lua stack to its construction-time height on scope exit. Provider
// interop calls push a function + arguments before a protected call; when the call
// is skipped (e.g. the plugin lookup returns null so PCall never runs) those slots
// would otherwise leak and accumulate across repeated queries until the stack
// overflows. Declaring one of these right after acquiring the provider state makes
// every early return — success or failure — stack-balanced.
class StackResetGuard {
 public:
  explicit StackResetGuard(lua_State* state) : state_(state), base_(lua_gettop(state)) {}
  ~StackResetGuard() { lua_settop(state_, base_); }
  StackResetGuard(const StackResetGuard&) = delete;
  StackResetGuard& operator=(const StackResetGuard&) = delete;

 private:
  lua_State* state_;
  int base_;
};

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
